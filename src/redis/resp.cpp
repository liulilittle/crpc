// CRPC -- Redis RESP protocol codec (implementation).
#include <crpc/redis/resp.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace crpc {
namespace redis {

namespace {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

constexpr std::byte cr_byte = static_cast<std::byte>('\r');
constexpr std::byte lf_byte = static_cast<std::byte>('\n');

// Lengths are validated in uint64_t before narrowing to size_t: on a 32-bit
// host a hostile length such as 2^32 + 10 would otherwise wrap past the limit
// and desynchronise the stream.
static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t),
              "resp requires size_t to be no wider than uint64_t");

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Strict base-10 parse. No sign other than a leading '-', no leading '+', no
// whitespace, no empty string. Rejects overflow instead of wrapping.
bool parse_i64(const char* data, std::size_t size, std::int64_t& out) noexcept {
    if (size == 0) {
        return false;
    }
    std::size_t i = 0;
    bool negative = false;
    if (data[0] == '-') {
        negative = true;
        i = 1;
        if (size == 1) {
            return false;
        }
    }
    const std::uint64_t limit =
        negative ? 9223372036854775808ull : 9223372036854775807ull;
    std::uint64_t value = 0;
    for (; i < size; ++i) {
        if (!is_digit(data[i])) {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(data[i] - '0');
        if (value > (limit - digit) / 10ull) {
            return false;
        }
        value = value * 10ull + digit;
    }
    if (negative) {
        out = (value == 9223372036854775808ull)
                  ? (std::numeric_limits<std::int64_t>::min)()
                  : -static_cast<std::int64_t>(value);
    } else {
        out = static_cast<std::int64_t>(value);
    }
    return true;
}

// Strict big-number validation: an optional leading '-' followed by digits.
bool is_big_number(const std::string& text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t i = text[0] == '-' ? 1 : 0;
    if (i >= text.size()) {
        return false;
    }
    for (; i < text.size(); ++i) {
        if (!is_digit(text[i])) {
            return false;
        }
    }
    return true;
}

// RESP simple strings and errors may not carry CR or LF; a line whose payload
// does is malformed rather than a value containing a control byte.
bool contains_crlf(const std::string& text) noexcept {
    return text.find('\r') != std::string::npos ||
           text.find('\n') != std::string::npos;
}

// A simple string or error is one line, so its payload must not carry CR or LF:
// the wire form would be a frame the parser above rejects, and a payload with an
// embedded CRLF would be read back as a prefix, silently dropping the rest. Two
// more types have a line form under one protocol: a RESP2 blob error falls back
// to an error line, and a RESP3 big number is `(` + text + CRLF. Both are checked
// only for that protocol, because the other protocol length-prefixes them.
// The check is recursive so a bad value nested in an array, set, push or map is
// caught before a single byte is written.
bool value_has_forbidden_crlf(const resp_value& value,
                              resp_protocol protocol) noexcept {
    // RESP3 attribute frames are only written under RESP3; a RESP2 encode drops
    // them, so their content does not reach the wire and needs no check.
    if (protocol == resp_protocol::resp3 && value.has_attributes()) {
        for (const auto& entry : value.attributes()) {
            if (value_has_forbidden_crlf(entry.first, protocol) ||
                value_has_forbidden_crlf(entry.second, protocol)) {
                return true;
            }
        }
    }
    switch (value.type()) {
    case resp_type::simple_string:
        return contains_crlf(value.as_simple_string());
    case resp_type::error:
        return contains_crlf(value.as_error());
    case resp_type::blob_error:
        return protocol == resp_protocol::resp2 &&
               contains_crlf(value.as_blob_error());
    case resp_type::big_number:
        // RESP3 spells a big number as a line (`(` + text + CRLF), so a payload
        // carrying CR/LF would split the frame; RESP2 falls back to a
        // length-prefixed bulk, where CR/LF is ordinary data.
        return protocol == resp_protocol::resp3 &&
               contains_crlf(value.as_big_number());
    case resp_type::array:
        for (const resp_value& item : value.as_array()) {
            if (value_has_forbidden_crlf(item, protocol)) {
                return true;
            }
        }
        return false;
    case resp_type::set_value:
        for (const resp_value& item : value.as_set()) {
            if (value_has_forbidden_crlf(item, protocol)) {
                return true;
            }
        }
        return false;
    case resp_type::push:
        for (const resp_value& item : value.as_push()) {
            if (value_has_forbidden_crlf(item, protocol)) {
                return true;
            }
        }
        return false;
    case resp_type::map_value:
        for (const auto& entry : value.as_map()) {
            if (value_has_forbidden_crlf(entry.first, protocol) ||
                value_has_forbidden_crlf(entry.second, protocol)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

// Accepts "inf", "-inf", "nan" and a plain decimal/scientific number. The
// character whitelist rejects whitespace and any other prefix strtod would
// silently skip.
bool parse_double(const std::string& text, double& out) noexcept {
    if (text.empty()) {
        return false;
    }
    if (text == "inf" || text == "+inf") {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "-inf") {
        out = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "nan" || text == "-nan" || text == "+nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    for (char c : text) {
        const bool ok = is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                        c == '-';
        if (!ok) {
            return false;
        }
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    // A finite input that overflows to infinity (e.g. "1e999") is not a valid
    // RESP double; Redis rejects it rather than rounding to inf.
    if (std::isinf(value)) {
        return false;
    }
    out = value;
    return true;
}

// Formats a double into `buffer` without allocating, so the noexcept size
// computation cannot terminate on bad_alloc. Returns the number of bytes.
std::size_t format_double(double value, char* buffer, std::size_t size) noexcept {
    if (std::isnan(value)) {
        if (size >= 3u) {
            std::memcpy(buffer, "nan", 3u);
        }
        return 3u;
    }
    if (std::isinf(value)) {
        if (value > 0.0) {
            if (size >= 3u) {
                std::memcpy(buffer, "inf", 3u);
            }
            return 3u;
        }
        if (size >= 4u) {
            std::memcpy(buffer, "-inf", 4u);
        }
        return 4u;
    }
    const int written = std::snprintf(buffer, size, "%.17g", value);
    return written > 0 ? static_cast<std::size_t>(written) : 0u;
}

// Formats an int64 into `buffer` without allocating. Returns the byte count.
std::size_t format_i64(std::int64_t value, char* buffer, std::size_t size) noexcept {
    const int written = std::snprintf(buffer, size, "%lld",
                                      static_cast<long long>(value));
    return written > 0 ? static_cast<std::size_t>(written) : 0u;
}

std::size_t digit_count(std::uint64_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10u) {
        value /= 10u;
        ++digits;
    }
    return digits;
}

void append_crlf(std::string& out) { out += "\r\n"; }

void append_bulk(std::string& out, const std::string& data) {
    out += '$';
    out += std::to_string(data.size());
    append_crlf(out);
    out += data;
    append_crlf(out);
}

// Writes one value in the wire form the negotiated protocol can express. The
// RESP2 fallback for a type RESP2 has no marker for is the closest RESP2 type:
// a null becomes `$-1`, a double/big number/verbatim string becomes a bulk
// string, and a map/set/push becomes an array. A RESP3 attribute prefix comes
// first; RESP2 has no attribute frame, so the value is written without it.
void encode_value_text(std::string& out, const resp_value& value,
                       resp_protocol protocol) {
    if (protocol == resp_protocol::resp3 && value.has_attributes()) {
        const resp_map& attributes = value.attributes();
        out += '|';
        out += std::to_string(attributes.size());
        append_crlf(out);
        for (const auto& entry : attributes) {
            encode_value_text(out, entry.first, protocol);
            encode_value_text(out, entry.second, protocol);
        }
    }
    switch (value.type()) {
    case resp_type::null_value:
        if (protocol == resp_protocol::resp3) {
            out += "_\r\n";
        } else {
            out += "$-1\r\n";
        }
        break;
    case resp_type::simple_string:
        out += '+';
        out += value.as_simple_string();
        append_crlf(out);
        break;
    case resp_type::error:
        out += '-';
        out += value.as_error();
        append_crlf(out);
        break;
    case resp_type::integer:
        out += ':';
        out += std::to_string(value.as_integer());
        append_crlf(out);
        break;
    case resp_type::bulk_string:
        append_bulk(out, value.as_bulk_string());
        break;
    case resp_type::array: {
        const resp_array& items = value.as_array();
        out += '*';
        out += std::to_string(items.size());
        append_crlf(out);
        for (const resp_value& item : items) {
            encode_value_text(out, item, protocol);
        }
        break;
    }
    case resp_type::boolean:
        if (protocol == resp_protocol::resp3) {
            out += value.as_boolean() ? "#t\r\n" : "#f\r\n";
        } else {
            out += ':';
            out += value.as_boolean() ? '1' : '0';
            append_crlf(out);
        }
        break;
    case resp_type::double_value: {
        char buffer[64];
        const std::size_t text_size =
            format_double(value.as_double(), buffer, sizeof(buffer));
        if (protocol == resp_protocol::resp3) {
            out += ',';
            out.append(buffer, text_size);
            append_crlf(out);
        } else {
            append_bulk(out, std::string(buffer, text_size));
        }
        break;
    }
    case resp_type::big_number:
        if (protocol == resp_protocol::resp3) {
            out += '(';
            out += value.as_big_number();
            append_crlf(out);
        } else {
            append_bulk(out, value.as_big_number());
        }
        break;
    case resp_type::blob_error:
        if (protocol == resp_protocol::resp3) {
            const std::string& message = value.as_blob_error();
            out += '!';
            out += std::to_string(message.size());
            append_crlf(out);
            out += message;
            append_crlf(out);
        } else {
            out += '-';
            out += value.as_blob_error();
            append_crlf(out);
        }
        break;
    case resp_type::verbatim_string: {
        if (protocol == resp_protocol::resp3) {
            const std::string& payload = value.as_verbatim_string();
            // The length covers the format, the separating colon and the
            // payload; the format is not assumed to be three bytes.
            const std::size_t total =
                value.verbatim_format().size() + 1u + payload.size();
            out += '=';
            out += std::to_string(total);
            append_crlf(out);
            out += value.verbatim_format();
            out += ':';
            out += payload;
            append_crlf(out);
        } else {
            append_bulk(out, value.as_verbatim_string());
        }
        break;
    }
    case resp_type::map_value: {
        const resp_map& entries = value.as_map();
        if (protocol == resp_protocol::resp3) {
            out += '%';
            out += std::to_string(entries.size());
            append_crlf(out);
        } else {
            out += '*';
            out += std::to_string(static_cast<std::uint64_t>(entries.size()) * 2u);
            append_crlf(out);
        }
        for (const auto& entry : entries) {
            encode_value_text(out, entry.first, protocol);
            encode_value_text(out, entry.second, protocol);
        }
        break;
    }
    case resp_type::set_value: {
        const resp_set& items = value.as_set();
        if (protocol == resp_protocol::resp3) {
            out += '~';
        } else {
            out += '*';
        }
        out += std::to_string(items.size());
        append_crlf(out);
        for (const resp_value& item : items) {
            encode_value_text(out, item, protocol);
        }
        break;
    }
    case resp_type::push: {
        const resp_push& items = value.as_push();
        if (protocol == resp_protocol::resp3) {
            out += '>';
        } else {
            out += '*';
        }
        out += std::to_string(items.size());
        append_crlf(out);
        for (const resp_value& item : items) {
            encode_value_text(out, item, protocol);
        }
        break;
    }
    }
}

} // namespace

// --- resp_value -------------------------------------------------------------

resp_type resp_value::type() const noexcept {
    switch (storage_.index()) {
    case 0:
        return resp_type::null_value;
    case 1:
        return resp_type::simple_string;
    case 2:
        return resp_type::error;
    case 3:
        return resp_type::integer;
    case 4:
        return resp_type::bulk_string;
    case 5:
        return resp_type::array;
    case 6:
        return resp_type::boolean;
    case 7:
        return resp_type::double_value;
    case 8:
        return resp_type::big_number;
    case 9:
        return resp_type::blob_error;
    case 10:
        return resp_type::verbatim_string;
    case 11:
        return resp_type::map_value;
    case 12:
        return resp_type::set_value;
    default:
        return resp_type::push;
    }
}

bool resp_value::is_null() const noexcept {
    return storage_.index() == 0;
}
bool resp_value::is_simple_string() const noexcept {
    return storage_.index() == 1;
}
bool resp_value::is_error() const noexcept {
    return storage_.index() == 2;
}
bool resp_value::is_integer() const noexcept {
    return storage_.index() == 3;
}
bool resp_value::is_bulk_string() const noexcept {
    return storage_.index() == 4;
}
bool resp_value::is_array() const noexcept {
    return storage_.index() == 5;
}
bool resp_value::is_boolean() const noexcept {
    return storage_.index() == 6;
}
bool resp_value::is_double() const noexcept {
    return storage_.index() == 7;
}
bool resp_value::is_big_number() const noexcept {
    return storage_.index() == 8;
}
bool resp_value::is_blob_error() const noexcept {
    return storage_.index() == 9;
}
bool resp_value::is_verbatim_string() const noexcept {
    return storage_.index() == 10;
}
bool resp_value::is_map() const noexcept {
    return storage_.index() == 11;
}
bool resp_value::is_set() const noexcept {
    return storage_.index() == 12;
}
bool resp_value::is_push() const noexcept {
    return storage_.index() == 13;
}

bool resp_value::has_attributes() const noexcept {
    return has_attributes_;
}

const resp_map& resp_value::attributes() const noexcept {
    return attributes_;
}

void resp_value::set_attributes(resp_map attributes) noexcept {
    attributes_ = std::move(attributes);
    has_attributes_ = true;
}

std::int64_t resp_value::as_integer() const noexcept {
    return std::get<resp_integer>(storage_).value;
}
bool resp_value::as_boolean() const noexcept {
    return std::get<resp_boolean>(storage_).value;
}
double resp_value::as_double() const noexcept {
    return std::get<resp_double>(storage_).value;
}
const std::string& resp_value::as_simple_string() const noexcept {
    return std::get<resp_simple_string>(storage_).value;
}
const std::string& resp_value::as_error() const noexcept {
    return std::get<resp_error>(storage_).message;
}
const std::string& resp_value::as_bulk_string() const noexcept {
    return std::get<resp_bulk_string>(storage_).value;
}
const std::string& resp_value::as_big_number() const noexcept {
    return std::get<resp_big_number>(storage_).value;
}
const std::string& resp_value::as_blob_error() const noexcept {
    return std::get<resp_blob_error>(storage_).message;
}
const std::string& resp_value::as_verbatim_string() const noexcept {
    return std::get<resp_verbatim_string>(storage_).value;
}
const std::string& resp_value::verbatim_format() const noexcept {
    return std::get<resp_verbatim_string>(storage_).format;
}
const resp_array& resp_value::as_array() const noexcept {
    return std::get<resp_array_value>(storage_).items;
}
const resp_set& resp_value::as_set() const noexcept {
    return std::get<resp_set_value>(storage_).items;
}
const resp_push& resp_value::as_push() const noexcept {
    return std::get<resp_push_value>(storage_).items;
}
const resp_map& resp_value::as_map() const noexcept {
    return std::get<resp_map_value>(storage_).entries;
}

bool operator==(const resp_null&, const resp_null&) noexcept { return true; }

bool operator==(const resp_simple_string& lhs, const resp_simple_string& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_error& lhs, const resp_error& rhs) noexcept {
    return lhs.message == rhs.message;
}

bool operator==(const resp_integer& lhs, const resp_integer& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_bulk_string& lhs, const resp_bulk_string& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_array_value& lhs, const resp_array_value& rhs) noexcept {
    return lhs.items == rhs.items;
}

bool operator==(const resp_boolean& lhs, const resp_boolean& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_double& lhs, const resp_double& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_big_number& lhs, const resp_big_number& rhs) noexcept {
    return lhs.value == rhs.value;
}

bool operator==(const resp_blob_error& lhs, const resp_blob_error& rhs) noexcept {
    return lhs.message == rhs.message;
}

bool operator==(const resp_verbatim_string& lhs,
                const resp_verbatim_string& rhs) noexcept {
    return lhs.value == rhs.value && lhs.format == rhs.format;
}

bool operator==(const resp_map_value& lhs, const resp_map_value& rhs) noexcept {
    return lhs.entries == rhs.entries;
}

bool operator==(const resp_set_value& lhs, const resp_set_value& rhs) noexcept {
    return lhs.items == rhs.items;
}

bool operator==(const resp_push_value& lhs, const resp_push_value& rhs) noexcept {
    return lhs.items == rhs.items;
}

bool operator==(const resp_value& lhs, const resp_value& rhs) noexcept {
    // Hand-written on purpose. std::variant::operator== would ask for
    // operator== on the alternatives, and the alternatives are implicitly
    // convertible to resp_value, so ADL would resolve that back to this
    // function and recurse without bound. Only the public type()/as_*()
    // accessors are used, so no friend declaration is needed and the shared
    // build keeps a single, consistent linkage.
    const resp_type type = lhs.type();
    if (type != rhs.type()) {
        return false;
    }
    if (lhs.has_attributes() != rhs.has_attributes()) {
        return false;
    }
    if (lhs.has_attributes() && lhs.attributes() != rhs.attributes()) {
        return false;
    }
    switch (type) {
    case resp_type::null_value:
        return true;
    case resp_type::simple_string:
        return lhs.as_simple_string() == rhs.as_simple_string();
    case resp_type::error:
        return lhs.as_error() == rhs.as_error();
    case resp_type::integer:
        return lhs.as_integer() == rhs.as_integer();
    case resp_type::bulk_string:
        return lhs.as_bulk_string() == rhs.as_bulk_string();
    case resp_type::array:
        return lhs.as_array() == rhs.as_array();
    case resp_type::boolean:
        return lhs.as_boolean() == rhs.as_boolean();
    case resp_type::double_value:
        return lhs.as_double() == rhs.as_double();
    case resp_type::big_number:
        return lhs.as_big_number() == rhs.as_big_number();
    case resp_type::blob_error:
        return lhs.as_blob_error() == rhs.as_blob_error();
    case resp_type::verbatim_string:
        return lhs.as_verbatim_string() == rhs.as_verbatim_string() &&
               lhs.verbatim_format() == rhs.verbatim_format();
    case resp_type::map_value:
        return lhs.as_map() == rhs.as_map();
    case resp_type::set_value:
        return lhs.as_set() == rhs.as_set();
    case resp_type::push:
        return lhs.as_push() == rhs.as_push();
    }
    return false;
}

bool operator!=(const resp_value& lhs, const resp_value& rhs) noexcept {
    return !(lhs == rhs);
}

// --- resp_parser ------------------------------------------------------------

resp_parser::resp_parser() noexcept = default;

resp_parser::resp_parser(std::size_t max_value_size) noexcept
    : max_value_size_(max_value_size) {}

resp_parser::resp_parser(std::size_t max_value_size, std::size_t max_depth) noexcept
    : max_value_size_(max_value_size), max_depth_(max_depth) {}

void resp_parser::feed(span<const std::byte> data) noexcept {
    if (failed()) {
        return;
    }
    if (data.empty()) {
        return;
    }
    try {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    } catch (const std::bad_alloc&) {
        error_ = rpc_code::resource_exhausted;
    }
}

resp_parser::parse_result resp_parser::read_line(std::size_t& pos, char& marker,
                                                 std::string& content) {
    if (pos >= buffer_.size()) {
        return parse_result::need_more;
    }
    // The line content (everything between the marker and the CRLF) is bounded
    // by max_value_size_. Without the bound a hostile peer could send an
    // unterminated line and force an unbounded scan and buffer growth.
    std::size_t end = npos;
    for (std::size_t i = pos + 1; i + 1 < buffer_.size(); ++i) {
        if (i - (pos + 1) > max_value_size_) {
            error_ = rpc_code::resource_exhausted;
            return parse_result::error;
        }
        if (buffer_[i] == cr_byte && buffer_[i + 1] == lf_byte) {
            end = i;
            break;
        }
    }
    if (end == npos) {
        return parse_result::need_more;
    }
    marker = static_cast<char>(buffer_[pos]);
    content.assign(reinterpret_cast<const char*>(buffer_.data() + pos + 1),
                   end - (pos + 1));
    pos = end + 2;
    return parse_result::ok;
}

void resp_parser::maybe_compact() noexcept {
    // Consumed bytes are only physically removed once a meaningful amount has
    // accumulated; removing after every value made a large pipeline O(n^2).
    if (offset_ < 65536u) {
        return;
    }
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
}

resp_parser::result resp_parser::next(resp_value& out) noexcept {
    if (failed()) {
        return result::error;
    }
    if (offset_ >= buffer_.size()) {
        return result::need_more;
    }
    try {
        std::size_t pos = offset_;
        resp_value value;
        const parse_result parsed = parse_value(pos, 0, value);
        if (parsed == parse_result::need_more) {
            return result::need_more;
        }
        if (parsed == parse_result::error) {
            return result::error;
        }
        out = std::move(value);
        offset_ = pos;
        maybe_compact();
        return result::value_ready;
    } catch (const std::bad_alloc&) {
        error_ = rpc_code::resource_exhausted;
        return result::error;
    }
}

status resp_parser::finish() noexcept {
    if (failed()) {
        return status(error_);
    }
    if (offset_ >= buffer_.size()) {
        return status();
    }
    // Unconsumed bytes remain. Decode them to tell a complete value (or a
    // sequence of complete values) from a truncated one, so finish() only
    // reports a protocol error for the latter. The scan uses a local cursor,
    // leaving offset_ and the buffer untouched.
    try {
        std::size_t pos = offset_;
        for (;;) {
            if (pos >= buffer_.size()) {
                break;
            }
            resp_value value;
            const parse_result parsed = parse_value(pos, 0, value);
            if (parsed == parse_result::need_more) {
                error_ = rpc_code::protocol_error;
                return status(error_);
            }
            if (parsed == parse_result::error) {
                return status(error_);
            }
        }
        return status();
    } catch (const std::bad_alloc&) {
        error_ = rpc_code::resource_exhausted;
        return status(error_);
    }
}

rpc_code resp_parser::error() const noexcept { return error_; }

bool resp_parser::failed() const noexcept { return error_ != rpc_code::ok; }

std::size_t resp_parser::buffered() const noexcept { return buffer_.size() - offset_; }

std::size_t resp_parser::max_value_size() const noexcept { return max_value_size_; }

std::size_t resp_parser::max_depth() const noexcept { return max_depth_; }

void resp_parser::reset() noexcept {
    buffer_.clear();
    offset_ = 0;
    error_ = rpc_code::ok;
}

resp_parser::parse_result resp_parser::parse_value(std::size_t& pos,
                                                    std::size_t depth,
                                                    resp_value& out) {
    if (depth > max_depth_) {
        error_ = rpc_code::resource_exhausted;
        return parse_result::error;
    }
    if (pos >= buffer_.size()) {
        return parse_result::need_more;
    }

    char marker = static_cast<char>(buffer_[pos]);
    std::string content;
    switch (marker) {
    case '+': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (contains_crlf(content)) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_simple_string{std::move(content)}};
        return parse_result::ok;
    }
    case '-': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (contains_crlf(content)) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_error{std::move(content)}};
        return parse_result::ok;
    }
    case ':': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        std::int64_t number = 0;
        if (!parse_i64(content.data(), content.size(), number)) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_integer{number}};
        return parse_result::ok;
    }
    case '_': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (!content.empty()) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_null{}};
        return parse_result::ok;
    }
    case '#': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (content == "t") {
            out = resp_value{resp_boolean{true}};
            return parse_result::ok;
        }
        if (content == "f") {
            out = resp_value{resp_boolean{false}};
            return parse_result::ok;
        }
        error_ = rpc_code::protocol_error;
        return parse_result::error;
    }
    case ',': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        double number = 0.0;
        if (!parse_double(content, number)) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_double{number}};
        return parse_result::ok;
    }
    case '(': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (!is_big_number(content)) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        out = resp_value{resp_big_number{std::move(content)}};
        return parse_result::ok;
    }
    case '$':
    case '!':
    case '=': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (marker == '$' && content == "?") {
            // Streamed string (RESP3, spec section "Streamed strings"). The
            // wire form is `$?` then `;<len>\r\n<data>\r\n` chunks repeated
            // any number of times, terminated by `;0\r\n`; the spec spells
            // "Hello world" as `$? ;4 Hell ;5 o wor ;1 d ;0`. The decoder
            // materialises it into one bulk string, concatenating with the
            // same total-length cap that bounds a fixed-length bulk.
            std::string data;
            for (;;) {
                char chunk_marker = 0;
                std::string chunk_line;
                const parse_result chunk = read_line(pos, chunk_marker, content);
                if (chunk != parse_result::ok) {
                    return chunk;
                }
                if (chunk_marker != ';') {
                    error_ = rpc_code::protocol_error;
                    return parse_result::error;
                }
                std::int64_t chunk_size = 0;
                if (!parse_i64(content.data(), content.size(), chunk_size) ||
                    chunk_size < 0) {
                    error_ = rpc_code::protocol_error;
                    return parse_result::error;
                }
                if (chunk_size == 0) {
                    break;
                }
                if (static_cast<std::uint64_t>(data.size()) +
                        static_cast<std::uint64_t>(chunk_size) >
                    static_cast<std::uint64_t>(max_value_size_)) {
                    error_ = rpc_code::resource_exhausted;
                    return parse_result::error;
                }
                const std::size_t count = static_cast<std::size_t>(chunk_size);
                if (count > buffer_.size() - pos ||
                    buffer_.size() - pos - count < 2) {
                    return parse_result::need_more;
                }
                const std::size_t data_end = pos + count;
                if (buffer_[data_end] != cr_byte ||
                    buffer_[data_end + 1] != lf_byte) {
                    error_ = rpc_code::protocol_error;
                    return parse_result::error;
                }
                data.append(reinterpret_cast<const char*>(buffer_.data() + pos),
                            count);
                pos = data_end + 2;
            }
            out = resp_value{resp_bulk_string{std::move(data)}};
            return parse_result::ok;
        }
        std::int64_t length = 0;
        if (!parse_i64(content.data(), content.size(), length) || length < -1) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        if (length == -1) {
            out = resp_value{resp_null{}};
            return parse_result::ok;
        }
        // Compare in uint64_t before narrowing: on a 32-bit host a length such
        // as 2^32 + 10 would wrap to 10 and slip past the limit.
        if (static_cast<std::uint64_t>(length) >
            static_cast<std::uint64_t>(max_value_size_)) {
            error_ = rpc_code::resource_exhausted;
            return parse_result::error;
        }
        const std::size_t count = static_cast<std::size_t>(length);
        // No `count + 2` here: it could wrap on a 32-bit host.
        if (count > buffer_.size() - pos || buffer_.size() - pos - count < 2) {
            return parse_result::need_more;
        }
        const std::size_t data_end = pos + count;
        if (buffer_[data_end] != cr_byte || buffer_[data_end + 1] != lf_byte) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        const std::string data(reinterpret_cast<const char*>(buffer_.data() + pos),
                               count);
        pos = data_end + 2;
        if (marker == '$') {
            out = resp_value{resp_bulk_string{std::move(data)}};
        } else if (marker == '!') {
            out = resp_value{resp_blob_error{std::move(data)}};
        } else {
            // Verbatim: <format>:<content> where the length covers both and
            // the separating colon.
            if (count < 4) {
                error_ = rpc_code::protocol_error;
                return parse_result::error;
            }
            if (data[3] != ':') {
                error_ = rpc_code::protocol_error;
                return parse_result::error;
            }
            out = resp_value{resp_verbatim_string{
                data.substr(4), data.substr(0, 3)}};
        }
        return parse_result::ok;
    }
    case '*':
    case '%':
    case '~':
    case '>': {
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        if (content == "?") {
            // Streamed aggregate (RESP3, spec section "Streamed aggregated
            // data types"): `*?`/`~?`/`%?` then elements, then a lone `.`
            // line as the END type. The map form must pair elements up, so a
            // terminator mid-pair is a protocol error. The decoder
            // materialises the aggregate into the ordinary concrete value.
            if (marker == '>') {
                error_ = rpc_code::protocol_error;
                return parse_result::error;
            }
            if (marker == '%') {
                resp_map entries;
                for (;;) {
                    if (pos >= buffer_.size()) {
                        return parse_result::need_more;
                    }
                    if (static_cast<char>(buffer_[pos]) == '.') {
                        break;
                    }
                    resp_value key;
                    const parse_result key_result =
                        parse_value(pos, depth + 1, key);
                    if (key_result != parse_result::ok) {
                        return key_result;
                    }
                    resp_value value;
                    const parse_result value_result =
                        parse_value(pos, depth + 1, value);
                    if (value_result != parse_result::ok) {
                        return value_result;
                    }
                    entries.emplace_back(std::move(key), std::move(value));
                }
                std::string end_line;
                char end_marker = 0;
                const parse_result end = read_line(pos, end_marker, end_line);
                if (end != parse_result::ok) {
                    return end;
                }
                if (end_marker != '.' || !end_line.empty()) {
                    error_ = rpc_code::protocol_error;
                    return parse_result::error;
                }
                out = resp_value{resp_map_value{std::move(entries)}};
                return parse_result::ok;
            }
            resp_array items;
            for (;;) {
                if (pos >= buffer_.size()) {
                    return parse_result::need_more;
                }
                if (static_cast<char>(buffer_[pos]) == '.') {
                    break;
                }
                resp_value item;
                const parse_result item_result =
                    parse_value(pos, depth + 1, item);
                if (item_result != parse_result::ok) {
                    return item_result;
                }
                items.push_back(std::move(item));
            }
            std::string end_line;
            char end_marker = 0;
            const parse_result end = read_line(pos, end_marker, end_line);
            if (end != parse_result::ok) {
                return end;
            }
            if (end_marker != '.' || !end_line.empty()) {
                error_ = rpc_code::protocol_error;
                return parse_result::error;
            }
            if (marker == '*') {
                out = resp_value{resp_array_value{std::move(items)}};
            } else {
                out = resp_value{resp_set_value{std::move(items)}};
            }
            return parse_result::ok;
        }
        std::int64_t count = 0;
        if (!parse_i64(content.data(), content.size(), count) || count < -1) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        if (count == -1) {
            out = resp_value{resp_null{}};
            return parse_result::ok;
        }
        // Compare in uint64_t before narrowing, as for bulk lengths above.
        if (static_cast<std::uint64_t>(count) >
            static_cast<std::uint64_t>(max_value_size_)) {
            error_ = rpc_code::resource_exhausted;
            return parse_result::error;
        }
        const std::size_t elements = static_cast<std::size_t>(count);
        if (marker == '%') {
            resp_map entries;
            if (elements <= 4096u) {
                entries.reserve(elements);
            }
            for (std::size_t i = 0; i < elements; ++i) {
                resp_value key;
                const parse_result key_result = parse_value(pos, depth + 1, key);
                if (key_result != parse_result::ok) {
                    return key_result;
                }
                resp_value value;
                const parse_result value_result = parse_value(pos, depth + 1, value);
                if (value_result != parse_result::ok) {
                    return value_result;
                }
                entries.emplace_back(std::move(key), std::move(value));
            }
            out = resp_value{resp_map_value{std::move(entries)}};
            return parse_result::ok;
        }
        resp_array items;
        if (elements <= 4096u) {
            items.reserve(elements);
        }
        for (std::size_t i = 0; i < elements; ++i) {
            resp_value item;
            const parse_result item_result = parse_value(pos, depth + 1, item);
            if (item_result != parse_result::ok) {
                return item_result;
            }
            items.push_back(std::move(item));
        }
        if (marker == '*') {
            out = resp_value{resp_array_value{std::move(items)}};
        } else if (marker == '~') {
            out = resp_value{resp_set_value{std::move(items)}};
        } else {
            out = resp_value{resp_push_value{std::move(items)}};
        }
        return parse_result::ok;
    }
    case '|': {
        // RESP3 attribute (spec section "Attribute type"): exactly like a map
        // (`|<count>` then count key/value pairs) but it is metadata that
        // precedes -- and augments -- the value that follows, which may itself
        // be any value, inside or outside an aggregate. The attribute is not a
        // value: it is folded onto the following value, and a caller that does
        // not care reads the reply as if it were never there.
        const parse_result line = read_line(pos, marker, content);
        if (line != parse_result::ok) {
            return line;
        }
        std::int64_t count = 0;
        if (!parse_i64(content.data(), content.size(), count) || count < 0) {
            error_ = rpc_code::protocol_error;
            return parse_result::error;
        }
        if (static_cast<std::uint64_t>(count) >
            static_cast<std::uint64_t>(max_value_size_)) {
            error_ = rpc_code::resource_exhausted;
            return parse_result::error;
        }
        const std::size_t elements = static_cast<std::size_t>(count);
        resp_map attributes;
        if (elements <= 4096u) {
            attributes.reserve(elements);
        }
        for (std::size_t i = 0; i < elements; ++i) {
            resp_value key;
            const parse_result key_result = parse_value(pos, depth + 1, key);
            if (key_result != parse_result::ok) {
                return key_result;
            }
            resp_value value;
            const parse_result value_result = parse_value(pos, depth + 1, value);
            if (value_result != parse_result::ok) {
                return value_result;
            }
            attributes.emplace_back(std::move(key), std::move(value));
        }
        // The attribute augments the value that immediately follows it. The
        // augmented value is parsed at the same depth the attribute occupies.
        resp_value augmented;
        const parse_result augmented_result =
            parse_value(pos, depth, augmented);
        if (augmented_result != parse_result::ok) {
            return augmented_result;
        }
        augmented.set_attributes(std::move(attributes));
        out = std::move(augmented);
        return parse_result::ok;
    }
    default:
        error_ = rpc_code::protocol_error;
        return parse_result::error;
    }
}

// --- resp_encoder -----------------------------------------------------------

resp_encoder::resp_encoder(resp_protocol protocol) noexcept : protocol_(protocol) {}

void resp_encoder::protocol(resp_protocol value) noexcept { protocol_ = value; }

resp_protocol resp_encoder::protocol() const noexcept { return protocol_; }

std::size_t resp_encoder::value_size(const resp_value& value) const noexcept {
    // The attribute prefix is RESP3-only. It must be counted exactly as
    // encode_value_text writes it, or the size/encode agreement check fails.
    std::size_t attribute_size = 0;
    if (protocol_ == resp_protocol::resp3 && value.has_attributes()) {
        const resp_map& attributes = value.attributes();
        attribute_size = digit_count(attributes.size()) + 3u;
        for (const auto& entry : attributes) {
            attribute_size += value_size(entry.first) + value_size(entry.second);
        }
    }
    return attribute_size + value_body_size(value);
}

std::size_t resp_encoder::value_body_size(const resp_value& value) const noexcept {
    switch (value.type()) {
    case resp_type::null_value:
        // RESP3 `_` + CRLF is 3 bytes; the RESP2 fallback `$-1` + CRLF is 5.
        return protocol_ == resp_protocol::resp3 ? 3u : 5u;
    case resp_type::simple_string:
        return value.as_simple_string().size() + 3u;
    case resp_type::error:
        return value.as_error().size() + 3u;
    case resp_type::integer: {
        // A stack buffer keeps value_size() allocation-free, so its noexcept
        // contract cannot terminate on bad_alloc.
        char buffer[32];
        const std::size_t digits =
            format_i64(value.as_integer(), buffer, sizeof(buffer));
        return digits + 3u;
    }
    case resp_type::bulk_string: {
        const std::size_t size = value.as_bulk_string().size();
        return digit_count(size) + 5u + size;
    }
    case resp_type::array: {
        const resp_array& items = value.as_array();
        std::size_t total = digit_count(items.size()) + 3u;
        for (const resp_value& item : items) {
            total += value_size(item);
        }
        return total;
    }
    case resp_type::boolean:
        return 4u;
    case resp_type::double_value: {
        char buffer[64];
        const std::size_t text_size =
            format_double(value.as_double(), buffer, sizeof(buffer));
        if (protocol_ == resp_protocol::resp3) {
            return text_size + 3u;
        }
        return digit_count(text_size) + 5u + text_size;
    }
    case resp_type::big_number: {
        const std::size_t size = value.as_big_number().size();
        if (protocol_ == resp_protocol::resp3) {
            return size + 3u;
        }
        return digit_count(size) + 5u + size;
    }
    case resp_type::blob_error: {
        const std::size_t size = value.as_blob_error().size();
        if (protocol_ == resp_protocol::resp3) {
            return digit_count(size) + 5u + size;
        }
        return size + 3u;
    }
    case resp_type::verbatim_string: {
        const std::size_t payload = value.as_verbatim_string().size();
        const std::size_t total = value.verbatim_format().size() + 1u + payload;
        if (protocol_ == resp_protocol::resp3) {
            return digit_count(total) + 5u + total;
        }
        return digit_count(payload) + 5u + payload;
    }
    case resp_type::map_value: {
        const resp_map& entries = value.as_map();
        if (protocol_ == resp_protocol::resp3) {
            std::size_t total = digit_count(entries.size()) + 3u;
            for (const auto& entry : entries) {
                total += value_size(entry.first) + value_size(entry.second);
            }
            return total;
        }
        std::size_t total =
            digit_count(static_cast<std::uint64_t>(entries.size()) * 2u) + 3u;
        for (const auto& entry : entries) {
            total += value_size(entry.first) + value_size(entry.second);
        }
        return total;
    }
    case resp_type::set_value: {
        const resp_set& items = value.as_set();
        std::size_t total = digit_count(items.size()) + 3u;
        for (const resp_value& item : items) {
            total += value_size(item);
        }
        return total;
    }
    case resp_type::push: {
        const resp_push& items = value.as_push();
        std::size_t total = digit_count(items.size()) + 3u;
        for (const resp_value& item : items) {
            total += value_size(item);
        }
        return total;
    }
    }
    return 0u;
}

std::size_t resp_encoder::encoded_size(const resp_value& value) const noexcept {
    return value_size(value);
}

std::size_t resp_encoder::encoded_size(const resp_command& command) const noexcept {
    std::size_t total = digit_count(command.size()) + 3u;
    for (const std::string& argument : command) {
        total += digit_count(argument.size()) + 5u + argument.size();
    }
    return total;
}

status resp_encoder::encode(span<std::byte> out, const resp_value& value,
                            std::size_t& written) const {
    if (value_has_forbidden_crlf(value, protocol_)) {
        written = 0;
        return status(rpc_code::encode_error);
    }
    try {
        const std::size_t declared = value_size(value);
        std::string text;
        text.reserve(declared);
        encode_value_text(text, value, protocol_);
        // The size computation and the encoder must agree; a mismatch would be
        // a silent over-read, so it is a hard error rather than a memcpy of the
        // declared size.
        if (text.size() != declared) {
            written = 0;
            return status(rpc_code::encode_error);
        }
        if (out.size() < text.size()) {
            written = 0;
            return status(rpc_code::encode_error);
        }
        if (!text.empty()) {
            std::memcpy(out.data(), text.data(), text.size());
        }
        written = text.size();
        return status();
    } catch (const std::bad_alloc&) {
        written = 0;
        return status(rpc_code::resource_exhausted);
    }
}

status resp_encoder::encode(span<std::byte> out, const resp_command& command,
                            std::size_t& written) const {
    try {
        const std::size_t declared = encoded_size(command);
        std::string text;
        text.reserve(declared);
        text += '*';
        text += std::to_string(command.size());
        append_crlf(text);
        for (const std::string& argument : command) {
            append_bulk(text, argument);
        }
        if (text.size() != declared) {
            written = 0;
            return status(rpc_code::encode_error);
        }
        if (out.size() < text.size()) {
            written = 0;
            return status(rpc_code::encode_error);
        }
        if (!text.empty()) {
            std::memcpy(out.data(), text.data(), text.size());
        }
        written = text.size();
        return status();
    } catch (const std::bad_alloc&) {
        written = 0;
        return status(rpc_code::resource_exhausted);
    }
}

std::string resp_encoder::encode_to_string(const resp_value& value) const {
    if (value_has_forbidden_crlf(value, protocol_)) {
        throw std::invalid_argument(
            "resp simple string/error payload must not contain CR or LF");
    }
    std::string text;
    text.reserve(value_size(value));
    encode_value_text(text, value, protocol_);
    return text;
}

std::string resp_encoder::encode_command_to_string(const resp_command& command) const {
    std::string text;
    text.reserve(encoded_size(command));
    text += '*';
    text += std::to_string(command.size());
    append_crlf(text);
    for (const std::string& argument : command) {
        append_bulk(text, argument);
    }
    return text;
}

// --- Free functions ---------------------------------------------------------

std::size_t encoded_value_size(const resp_value& value, resp_protocol protocol) noexcept {
    return resp_encoder{protocol}.encoded_size(value);
}

std::size_t encoded_command_size(const resp_command& command) noexcept {
    return resp_encoder{}.encoded_size(command);
}

status encode_value(span<std::byte> out, const resp_value& value, std::size_t& written,
                    resp_protocol protocol) {
    return resp_encoder{protocol}.encode(out, value, written);
}

status encode_command(span<std::byte> out, const resp_command& command,
                      std::size_t& written) {
    return resp_encoder{}.encode(out, command, written);
}

std::string encode_value_to_string(const resp_value& value, resp_protocol protocol) {
    return resp_encoder{protocol}.encode_to_string(value);
}

std::string encode_command_to_string(const resp_command& command) {
    return resp_encoder{}.encode_command_to_string(command);
}

} // namespace redis
} // namespace crpc
