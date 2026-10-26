// CRPC -- Redis data-type layer implementation.
//
// This translation unit implements every symbol declared in types.hpp: the
// `value` accessors, the RESP3 wire codec for the `value` model
// (encode_value/decode_value), the RESP2 command encoder (encode_command), the
// typed reply projections (decode_*), the per-type command builders and the
// input iterators.
//
// The codec here is deliberately self-contained: it speaks RESP directly to the
// `value` model rather than round-tripping through resp.hpp's resp_value, so
// the data-type layer keeps its own wire contract.
#include <crpc/redis/types.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace crpc {
namespace redis {

namespace {

// --- number formatting / parsing --------------------------------------------

// Formats a double without allocating, matching the wire codec's "%.17g".
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

std::string format_real(double value) {
    char buffer[64];
    const std::size_t size = format_double(value, buffer, sizeof(buffer));
    return std::string(buffer, size);
}

// Strict integer parse: an optional '-' followed by digits, no sign for zero
// padding surprises, no trailing garbage.
bool parse_i64(const std::string& text, i64& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

// Strict double parse: the RESP spellings for the non-finite values, otherwise
// a character whitelist plus a full-consumption strtod. A finite input that
// overflows to infinity is rejected, as the wire codec does.
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
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                        c == '+' || c == '-';
        if (!ok) {
            return false;
        }
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    if (std::isinf(value)) {
        return false;
    }
    out = value;
    return true;
}

// Flattens a scalar `value` into a command argument. Non-scalar kinds have no
// Redis scalar spelling, so they are a programming error.
std::string value_arg(const value& v) {
    switch (v.kind()) {
    case value_kind::string:
        return v.as_string();
    case value_kind::integer:
        return std::to_string(v.as_int());
    case value_kind::real:
        return format_real(v.as_real());
    case value_kind::boolean:
        return v.as_bool() ? "1" : "0";
    default:
        throw std::bad_variant_access{};
    }
}

// --- RESP encoding ----------------------------------------------------------

void append_crlf(std::string& out) { out += "\r\n"; }

void append_bulk(std::string& out, const std::string& data) {
    out += '$';
    out += std::to_string(data.size());
    append_crlf(out);
    out += data;
    append_crlf(out);
}

// Writes one value in RESP3 form. A string is always a bulk string so binary
// payloads survive; a map/set/push use their RESP3 markers.
void encode_value_text(std::string& out, const value& v) {
    switch (v.kind()) {
    case value_kind::null:
        out += "_\r\n";
        break;
    case value_kind::boolean:
        out += v.as_bool() ? "#t\r\n" : "#f\r\n";
        break;
    case value_kind::integer:
        out += ':';
        out += std::to_string(v.as_int());
        append_crlf(out);
        break;
    case value_kind::real: {
        char buffer[64];
        const std::size_t size = format_double(v.as_real(), buffer, sizeof(buffer));
        out += ',';
        out.append(buffer, size);
        append_crlf(out);
        break;
    }
    case value_kind::string:
        append_bulk(out, v.as_string());
        break;
    case value_kind::array: {
        const array_value& items = v.as_array();
        out += '*';
        out += std::to_string(items.size());
        append_crlf(out);
        for (const value& item : items) {
            encode_value_text(out, item);
        }
        break;
    }
    case value_kind::map: {
        const map_value& entries = v.as_map();
        out += '%';
        out += std::to_string(entries.size());
        append_crlf(out);
        for (const auto& entry : entries) {
            encode_value_text(out, entry.first);
            encode_value_text(out, entry.second);
        }
        break;
    }
    case value_kind::set: {
        const std::vector<value>& items = v.as_set().items;
        out += '~';
        out += std::to_string(items.size());
        append_crlf(out);
        for (const value& item : items) {
            encode_value_text(out, item);
        }
        break;
    }
    case value_kind::push: {
        const std::vector<value>& items = v.as_push().items;
        out += '>';
        out += std::to_string(items.size());
        append_crlf(out);
        for (const value& item : items) {
            encode_value_text(out, item);
        }
        break;
    }
    case value_kind::error:
        out += '-';
        out += v.as_error().message;
        append_crlf(out);
        break;
    }
}

// --- RESP decoding ----------------------------------------------------------

class value_decoder {
public:
    explicit value_decoder(const std::string& bytes) : bytes_(bytes) {}

    bool parse(value& out) { return parse_value(out); }
    std::size_t position() const noexcept { return pos_; }

private:
    bool peek(char& c) const {
        if (pos_ >= bytes_.size()) {
            return false;
        }
        c = bytes_[pos_];
        return true;
    }

    bool expect_crlf() {
        if (pos_ + 1 >= bytes_.size()) {
            return false;
        }
        if (bytes_[pos_] != '\r' || bytes_[pos_ + 1] != '\n') {
            return false;
        }
        pos_ += 2;
        return true;
    }

    bool read_line(std::string& out) {
        const std::size_t newline = bytes_.find("\r\n", pos_);
        if (newline == std::string::npos) {
            return false;
        }
        out.assign(bytes_, pos_, newline - pos_);
        pos_ = newline + 2;
        return true;
    }

    bool read_count(i64& count) {
        std::string line;
        if (!read_line(line)) {
            return false;
        }
        return parse_i64(line, count);
    }

    bool read_bulk_body(std::size_t count, std::string& data) {
        if (bytes_.size() - pos_ < count + 2u) {
            return false;
        }
        data.assign(bytes_, pos_, count);
        pos_ += count;
        return expect_crlf();
    }

    bool parse_value(value& out) {
        char marker = 0;
        if (!peek(marker)) {
            return false;
        }
        switch (marker) {
        case '_':
            ++pos_;
            if (!expect_crlf()) {
                return false;
            }
            out = value{nullptr};
            return true;
        case '#': {
            ++pos_;
            char bit = 0;
            if (!peek(bit)) {
                return false;
            }
            ++pos_;
            if (!expect_crlf()) {
                return false;
            }
            if (bit == 't') {
                out = value{true};
                return true;
            }
            if (bit == 'f') {
                out = value{false};
                return true;
            }
            return false;
        }
        case ':': {
            ++pos_;
            i64 number = 0;
            if (!read_count(number)) {
                return false;
            }
            out = value{number};
            return true;
        }
        case ',': {
            ++pos_;
            std::string line;
            if (!read_line(line)) {
                return false;
            }
            double number = 0.0;
            if (!parse_double(line, number)) {
                return false;
            }
            out = value{number};
            return true;
        }
        case '$': {
            ++pos_;
            i64 length = 0;
            if (!read_count(length)) {
                return false;
            }
            if (length == -1) {
                out = value{nullptr};
                return true;
            }
            if (length < 0 || static_cast<std::uint64_t>(length) > max_size) {
                return false;
            }
            std::string data;
            if (!read_bulk_body(static_cast<std::size_t>(length), data)) {
                return false;
            }
            out = value{std::move(data)};
            return true;
        }
        case '+': {
            ++pos_;
            std::string line;
            if (!read_line(line)) {
                return false;
            }
            out = value{std::move(line)};
            return true;
        }
        case '-': {
            ++pos_;
            std::string line;
            if (!read_line(line)) {
                return false;
            }
            out = value{error_value{std::move(line)}};
            return true;
        }
        case '*': {
            ++pos_;
            i64 count = 0;
            if (!read_count(count)) {
                return false;
            }
            if (count == -1) {
                out = value{nullptr};
                return true;
            }
            if (count < 0 || depth_ >= max_depth) {
                return false;
            }
            array_value items;
            if (static_cast<std::size_t>(count) <= reserve_cap) {
                items.reserve(static_cast<std::size_t>(count));
            }
            ++depth_;
            for (i64 i = 0; i < count; ++i) {
                value item;
                if (!parse_value(item)) {
                    --depth_;
                    return false;
                }
                items.push_back(std::move(item));
            }
            --depth_;
            out = value{std::move(items)};
            return true;
        }
        case '%': {
            ++pos_;
            i64 count = 0;
            if (!read_count(count)) {
                return false;
            }
            if (count < 0 || depth_ >= max_depth) {
                return false;
            }
            map_value entries;
            if (static_cast<std::size_t>(count) <= reserve_cap) {
                entries.reserve(static_cast<std::size_t>(count));
            }
            ++depth_;
            for (i64 i = 0; i < count; ++i) {
                value key;
                value val;
                if (!parse_value(key) || !parse_value(val)) {
                    --depth_;
                    return false;
                }
                entries.emplace_back(std::move(key), std::move(val));
            }
            --depth_;
            out = value{std::move(entries)};
            return true;
        }
        case '~': {
            ++pos_;
            i64 count = 0;
            if (!read_count(count)) {
                return false;
            }
            if (count < 0 || depth_ >= max_depth) {
                return false;
            }
            std::vector<value> items;
            if (static_cast<std::size_t>(count) <= reserve_cap) {
                items.reserve(static_cast<std::size_t>(count));
            }
            ++depth_;
            for (i64 i = 0; i < count; ++i) {
                value item;
                if (!parse_value(item)) {
                    --depth_;
                    return false;
                }
                items.push_back(std::move(item));
            }
            --depth_;
            out = value{set_value{std::move(items)}};
            return true;
        }
        case '>': {
            ++pos_;
            i64 count = 0;
            if (!read_count(count)) {
                return false;
            }
            if (count < 0 || depth_ >= max_depth) {
                return false;
            }
            std::vector<value> items;
            if (static_cast<std::size_t>(count) <= reserve_cap) {
                items.reserve(static_cast<std::size_t>(count));
            }
            ++depth_;
            for (i64 i = 0; i < count; ++i) {
                value item;
                if (!parse_value(item)) {
                    --depth_;
                    return false;
                }
                items.push_back(std::move(item));
            }
            --depth_;
            out = value{push_value{std::move(items)}};
            return true;
        }
        case '(': {
            ++pos_;
            std::string line;
            if (!read_line(line)) {
                return false;
            }
            out = value{std::move(line)};
            return true;
        }
        case '!': {
            ++pos_;
            i64 length = 0;
            if (!read_count(length)) {
                return false;
            }
            if (length < 0 || static_cast<std::uint64_t>(length) > max_size) {
                return false;
            }
            std::string message;
            if (!read_bulk_body(static_cast<std::size_t>(length), message)) {
                return false;
            }
            out = value{error_value{std::move(message)}};
            return true;
        }
        case '=': {
            ++pos_;
            i64 length = 0;
            if (!read_count(length)) {
                return false;
            }
            if (length < 0 || static_cast<std::uint64_t>(length) > max_size) {
                return false;
            }
            std::string content;
            if (!read_bulk_body(static_cast<std::size_t>(length), content)) {
                return false;
            }
            const std::size_t colon = content.find(':');
            if (colon == std::string::npos) {
                out = value{std::move(content)};
            } else {
                out = value{content.substr(colon + 1)};
            }
            return true;
        }
        default:
            return false;
        }
    }

    static constexpr std::size_t max_depth = 128;
    static constexpr std::size_t max_size = 512u * 1024u * 1024u;
    // A hostile count (e.g. `*1000000000000000000`) must not make the decoder
    // attempt an allocation proportional to it before any element is parsed.
    // The real element count is bounded by the bytes actually present, so the
    // vector grows as the input does; only small counts are worth pre-reserving.
    static constexpr std::size_t reserve_cap = 4096;

    const std::string& bytes_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
};

// --- reply projection helpers -----------------------------------------------

redis_stream_entry parse_stream_entry(const value& item) {
    const array_value& entry = item.as_array();
    if (entry.size() != 2) {
        throw std::bad_variant_access{};
    }
    redis_stream_entry result;
    result.id = decode_string(entry[0]);
    const array_value& fields = entry[1].as_array();
    if (fields.size() % 2 != 0) {
        throw std::bad_variant_access{};
    }
    result.fields.reserve(fields.size() / 2);
    for (std::size_t i = 0; i < fields.size(); i += 2) {
        result.fields.emplace_back(decode_string(fields[i]), decode_string(fields[i + 1]));
    }
    return result;
}

} // namespace

// --- value ------------------------------------------------------------------

value_kind value::kind() const noexcept {
    return static_cast<value_kind>(value_base::index());
}

bool value::is_null() const noexcept { return index() == 0; }
bool value::is_bool() const noexcept { return index() == 1; }
bool value::is_int() const noexcept { return index() == 2; }
bool value::is_real() const noexcept { return index() == 3; }
bool value::is_string() const noexcept { return index() == 4; }
bool value::is_array() const noexcept { return index() == 5; }
bool value::is_map() const noexcept { return index() == 6; }
bool value::is_set() const noexcept { return index() == 7; }
bool value::is_push() const noexcept { return index() == 8; }
bool value::is_error() const noexcept { return index() == 9; }

bool value::as_bool() const { return std::get<bool>(*this); }
i64 value::as_int() const { return std::get<i64>(*this); }
double value::as_real() const { return std::get<double>(*this); }
const std::string& value::as_string() const { return std::get<std::string>(*this); }
const array_value& value::as_array() const { return std::get<array_value>(*this); }
const map_value& value::as_map() const { return std::get<map_value>(*this); }
const set_value& value::as_set() const { return std::get<set_value>(*this); }
const push_value& value::as_push() const { return std::get<push_value>(*this); }
const error_value& value::as_error() const { return std::get<error_value>(*this); }

const bool* value::if_bool() const noexcept { return std::get_if<bool>(this); }
const i64* value::if_int() const noexcept { return std::get_if<i64>(this); }
const double* value::if_real() const noexcept { return std::get_if<double>(this); }
const std::string* value::if_string() const noexcept { return std::get_if<std::string>(this); }
const array_value* value::if_array() const noexcept { return std::get_if<array_value>(this); }
const map_value* value::if_map() const noexcept { return std::get_if<map_value>(this); }
const set_value* value::if_set() const noexcept { return std::get_if<set_value>(this); }
const push_value* value::if_push() const noexcept { return std::get_if<push_value>(this); }
const error_value* value::if_error() const noexcept { return std::get_if<error_value>(this); }

// --- wire interop -----------------------------------------------------------

std::string encode_command(const command& cmd) {
    std::string out;
    out += '*';
    out += std::to_string(cmd.size());
    append_crlf(out);
    for (const std::string& arg : cmd) {
        append_bulk(out, arg);
    }
    return out;
}

std::string encode_value(const value& v) {
    std::string out;
    encode_value_text(out, v);
    return out;
}

bool decode_value(const std::string& bytes, value& out, std::size_t& consumed) {
    value_decoder decoder(bytes);
    value result;
    if (!decoder.parse(result)) {
        consumed = 0;
        return false;
    }
    out = std::move(result);
    consumed = decoder.position();
    return true;
}

// --- reply decoders ---------------------------------------------------------

bool decode_bool(const value& v) {
    if (v.is_bool()) {
        return v.as_bool();
    }
    if (v.is_int()) {
        return v.as_int() != 0;
    }
    throw std::bad_variant_access{};
}

i64 decode_int(const value& v) {
    if (v.is_int()) {
        return v.as_int();
    }
    throw std::bad_variant_access{};
}

double decode_real(const value& v) {
    if (v.is_real()) {
        return v.as_real();
    }
    if (v.is_int()) {
        return static_cast<double>(v.as_int());
    }
    throw std::bad_variant_access{};
}

std::string decode_string(const value& v) {
    if (v.is_string()) {
        return v.as_string();
    }
    throw std::bad_variant_access{};
}

std::vector<std::string> decode_string_array(const value& v) {
    const array_value& items = v.as_array();
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const value& item : items) {
        if (!item.is_string()) {
            throw std::bad_variant_access{};
        }
        out.push_back(item.as_string());
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> decode_pairs(const value& v) {
    std::vector<std::pair<std::string, std::string>> out;
    if (v.is_map()) {
        const map_value& entries = v.as_map();
        out.reserve(entries.size());
        for (const auto& entry : entries) {
            if (!entry.first.is_string() || !entry.second.is_string()) {
                throw std::bad_variant_access{};
            }
            out.emplace_back(entry.first.as_string(), entry.second.as_string());
        }
        return out;
    }
    if (v.is_array()) {
        const array_value& items = v.as_array();
        if (items.size() % 2 != 0) {
            throw std::bad_variant_access{};
        }
        out.reserve(items.size() / 2);
        for (std::size_t i = 0; i < items.size(); i += 2) {
            if (!items[i].is_string() || !items[i + 1].is_string()) {
                throw std::bad_variant_access{};
            }
            out.emplace_back(items[i].as_string(), items[i + 1].as_string());
        }
        return out;
    }
    throw std::bad_variant_access{};
}

std::vector<double> decode_real_array(const value& v) {
    const array_value& items = v.as_array();
    std::vector<double> out;
    out.reserve(items.size());
    for (const value& item : items) {
        out.push_back(decode_real(item));
    }
    return out;
}

std::vector<scored_member> decode_scored_members(const value& v) {
    const array_value& items = v.as_array();
    std::vector<scored_member> out;
    // Redis returns ZRANGE WITHSCORES as a flat [member, score, ...] array;
    // a caller may also hand in an array of [member, score] pairs.
    if (!items.empty() && items[0].is_array()) {
        out.reserve(items.size());
        for (const value& item : items) {
            const array_value& pair = item.as_array();
            if (pair.size() != 2) {
                throw std::bad_variant_access{};
            }
            scored_member member;
            member.member = decode_string(pair[0]);
            member.score = decode_real(pair[1]);
            out.push_back(std::move(member));
        }
        return out;
    }
    if (items.size() % 2 != 0) {
        throw std::bad_variant_access{};
    }
    out.reserve(items.size() / 2);
    for (std::size_t i = 0; i < items.size(); i += 2) {
        scored_member member;
        member.member = decode_string(items[i]);
        member.score = decode_real(items[i + 1]);
        out.push_back(std::move(member));
    }
    return out;
}

std::vector<redis_stream_entry> decode_stream_entries(const value& v) {
    const array_value& items = v.as_array();
    std::vector<redis_stream_entry> out;
    out.reserve(items.size());
    for (const value& item : items) {
        out.push_back(parse_stream_entry(item));
    }
    return out;
}

// --- string -----------------------------------------------------------------

command string_type::set(std::string key, value val, std::optional<ttl> expire) {
    command cmd;
    cmd.reserve(5);
    cmd.push_back("SET");
    cmd.push_back(std::move(key));
    cmd.push_back(value_arg(val));
    if (expire.has_value()) {
        cmd.push_back("PX");
        cmd.push_back(std::to_string(expire->count()));
    }
    return cmd;
}

command string_type::setnx(std::string key, value val) {
    return command{"SETNX", std::move(key), value_arg(val)};
}

command string_type::setex(std::string key, std::chrono::seconds seconds, value val) {
    return command{"SETEX", std::move(key), std::to_string(seconds.count()),
                   value_arg(val)};
}

command string_type::psetex(std::string key, std::chrono::milliseconds millis,
                            value val) {
    return command{"PSETEX", std::move(key), std::to_string(millis.count()),
                   value_arg(val)};
}

command string_type::get(std::string key) { return command{"GET", std::move(key)}; }

command string_type::getset(std::string key, value val) {
    return command{"GETSET", std::move(key), value_arg(val)};
}

command string_type::getdel(std::string key) {
    return command{"GETDEL", std::move(key)};
}

command string_type::getex(std::string key, std::optional<ttl> expire) {
    command cmd;
    cmd.reserve(5);
    cmd.push_back("GETEX");
    cmd.push_back(std::move(key));
    if (expire.has_value()) {
        cmd.push_back("PX");
        cmd.push_back(std::to_string(expire->count()));
    }
    return cmd;
}

command string_type::append(std::string key, std::string val) {
    return command{"APPEND", std::move(key), std::move(val)};
}

command string_type::strlen(std::string key) {
    return command{"STRLEN", std::move(key)};
}

command string_type::incr(std::string key) { return command{"INCR", std::move(key)}; }

command string_type::decr(std::string key) { return command{"DECR", std::move(key)}; }

command string_type::incrby(std::string key, i64 amount) {
    return command{"INCRBY", std::move(key), std::to_string(amount)};
}

command string_type::decrby(std::string key, i64 amount) {
    return command{"DECRBY", std::move(key), std::to_string(amount)};
}

command string_type::incrbyfloat(std::string key, double amount) {
    return command{"INCRBYFLOAT", std::move(key), format_real(amount)};
}

command string_type::mget(std::vector<std::string> keys) {
    command cmd;
    cmd.reserve(keys.size() + 1);
    cmd.push_back("MGET");
    for (std::string& key : keys) {
        cmd.push_back(std::move(key));
    }
    return cmd;
}

command string_type::mset(std::vector<std::pair<std::string, std::string>> pairs) {
    command cmd;
    cmd.reserve(pairs.size() * 2 + 1);
    cmd.push_back("MSET");
    for (auto& pair : pairs) {
        cmd.push_back(std::move(pair.first));
        cmd.push_back(std::move(pair.second));
    }
    return cmd;
}

// --- list -------------------------------------------------------------------

namespace {

command push_values(const char* name, std::string key, std::vector<value> values) {
    command cmd;
    cmd.reserve(values.size() + 2);
    cmd.push_back(name);
    cmd.push_back(std::move(key));
    for (value& item : values) {
        cmd.push_back(value_arg(item));
    }
    return cmd;
}

command push_strings(const char* name, std::string key,
                     std::vector<std::string> values) {
    command cmd;
    cmd.reserve(values.size() + 2);
    cmd.push_back(name);
    cmd.push_back(std::move(key));
    for (std::string& item : values) {
        cmd.push_back(std::move(item));
    }
    return cmd;
}

command push_keys(const char* name, std::vector<std::string> keys) {
    command cmd;
    cmd.reserve(keys.size() + 1);
    cmd.push_back(name);
    for (std::string& key : keys) {
        cmd.push_back(std::move(key));
    }
    return cmd;
}

command store(const char* name, std::string destination, std::vector<std::string> keys) {
    command cmd;
    cmd.reserve(keys.size() + 2);
    cmd.push_back(name);
    cmd.push_back(std::move(destination));
    for (std::string& key : keys) {
        cmd.push_back(std::move(key));
    }
    return cmd;
}

} // namespace

command list_type::lpush(std::string key, std::vector<value> values) {
    return push_values("LPUSH", std::move(key), std::move(values));
}

command list_type::rpush(std::string key, std::vector<value> values) {
    return push_values("RPUSH", std::move(key), std::move(values));
}

command list_type::lpushx(std::string key, std::vector<value> values) {
    return push_values("LPUSHX", std::move(key), std::move(values));
}

command list_type::rpushx(std::string key, std::vector<value> values) {
    return push_values("RPUSHX", std::move(key), std::move(values));
}

command list_type::lpop(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("LPOP");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command list_type::rpop(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("RPOP");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command list_type::lrange(std::string key, i64 start, i64 stop) {
    return command{"LRANGE", std::move(key), std::to_string(start),
                   std::to_string(stop)};
}

command list_type::llen(std::string key) { return command{"LLEN", std::move(key)}; }

command list_type::lindex(std::string key, i64 index) {
    return command{"LINDEX", std::move(key), std::to_string(index)};
}

command list_type::lset(std::string key, i64 index, value val) {
    return command{"LSET", std::move(key), std::to_string(index), value_arg(val)};
}

command list_type::lrem(std::string key, i64 count, value val) {
    return command{"LREM", std::move(key), std::to_string(count), value_arg(val)};
}

command list_type::ltrim(std::string key, i64 start, i64 stop) {
    return command{"LTRIM", std::move(key), std::to_string(start),
                   std::to_string(stop)};
}

command list_type::lpos(std::string key, value val, std::optional<i64> rank,
                        std::optional<i64> count, std::optional<i64> maxlen) {
    command cmd;
    cmd.reserve(8);
    cmd.push_back("LPOS");
    cmd.push_back(std::move(key));
    cmd.push_back(value_arg(val));
    if (rank.has_value()) {
        cmd.push_back("RANK");
        cmd.push_back(std::to_string(*rank));
    }
    if (count.has_value()) {
        cmd.push_back("COUNT");
        cmd.push_back(std::to_string(*count));
    }
    if (maxlen.has_value()) {
        cmd.push_back("MAXLEN");
        cmd.push_back(std::to_string(*maxlen));
    }
    return cmd;
}

command list_type::rpoplpush(std::string source, std::string destination) {
    return command{"RPOPLPUSH", std::move(source), std::move(destination)};
}

// --- hash -------------------------------------------------------------------

command hash_type::hset(std::string key, std::string field, value val) {
    return command{"HSET", std::move(key), std::move(field), value_arg(val)};
}

command hash_type::hset(std::string key,
                        std::vector<std::pair<std::string, std::string>> pairs) {
    command cmd;
    cmd.reserve(pairs.size() * 2 + 2);
    cmd.push_back("HSET");
    cmd.push_back(std::move(key));
    for (auto& pair : pairs) {
        cmd.push_back(std::move(pair.first));
        cmd.push_back(std::move(pair.second));
    }
    return cmd;
}

command hash_type::hsetnx(std::string key, std::string field, value val) {
    return command{"HSETNX", std::move(key), std::move(field), value_arg(val)};
}

command hash_type::hget(std::string key, std::string field) {
    return command{"HGET", std::move(key), std::move(field)};
}

command hash_type::hdel(std::string key, std::vector<std::string> fields) {
    return push_strings("HDEL", std::move(key), std::move(fields));
}

command hash_type::hlen(std::string key) { return command{"HLEN", std::move(key)}; }

command hash_type::hexists(std::string key, std::string field) {
    return command{"HEXISTS", std::move(key), std::move(field)};
}

command hash_type::hkeys(std::string key) { return command{"HKEYS", std::move(key)}; }

command hash_type::hvals(std::string key) { return command{"HVALS", std::move(key)}; }

command hash_type::hgetall(std::string key) {
    return command{"HGETALL", std::move(key)};
}

command hash_type::hmget(std::string key, std::vector<std::string> fields) {
    return push_strings("HMGET", std::move(key), std::move(fields));
}

command hash_type::hincrby(std::string key, std::string field, i64 amount) {
    return command{"HINCRBY", std::move(key), std::move(field),
                   std::to_string(amount)};
}

command hash_type::hincrbyfloat(std::string key, std::string field, double amount) {
    return command{"HINCRBYFLOAT", std::move(key), std::move(field),
                   format_real(amount)};
}

command hash_type::hstrlen(std::string key, std::string field) {
    return command{"HSTRLEN", std::move(key), std::move(field)};
}

command hash_type::hrandfield(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("HRANDFIELD");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

// --- set --------------------------------------------------------------------

command set_type::sadd(std::string key, std::vector<std::string> members) {
    return push_strings("SADD", std::move(key), std::move(members));
}

command set_type::srem(std::string key, std::vector<std::string> members) {
    return push_strings("SREM", std::move(key), std::move(members));
}

command set_type::smembers(std::string key) {
    return command{"SMEMBERS", std::move(key)};
}

command set_type::sismember(std::string key, std::string member) {
    return command{"SISMEMBER", std::move(key), std::move(member)};
}

command set_type::smismember(std::string key, std::vector<std::string> members) {
    return push_strings("SMISMEMBER", std::move(key), std::move(members));
}

command set_type::scard(std::string key) { return command{"SCARD", std::move(key)}; }

command set_type::sinter(std::vector<std::string> keys) {
    return push_keys("SINTER", std::move(keys));
}

command set_type::sunion(std::vector<std::string> keys) {
    return push_keys("SUNION", std::move(keys));
}

command set_type::sdiff(std::vector<std::string> keys) {
    return push_keys("SDIFF", std::move(keys));
}

command set_type::sinterstore(std::string destination, std::vector<std::string> keys) {
    return store("SINTERSTORE", std::move(destination), std::move(keys));
}

command set_type::sunionstore(std::string destination, std::vector<std::string> keys) {
    return store("SUNIONSTORE", std::move(destination), std::move(keys));
}

command set_type::sdiffstore(std::string destination, std::vector<std::string> keys) {
    return store("SDIFFSTORE", std::move(destination), std::move(keys));
}

command set_type::spop(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("SPOP");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command set_type::srandmember(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("SRANDMEMBER");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command set_type::smove(std::string source, std::string destination,
                        std::string member) {
    return command{"SMOVE", std::move(source), std::move(destination),
                   std::move(member)};
}

// --- sorted set -------------------------------------------------------------

command zset_type::zadd(std::string key, std::vector<member> members) {
    command cmd;
    cmd.reserve(members.size() * 2 + 2);
    cmd.push_back("ZADD");
    cmd.push_back(std::move(key));
    for (const member& item : members) {
        cmd.push_back(format_real(item.score));
        cmd.push_back(item.value);
    }
    return cmd;
}

command zset_type::zadd_incr(std::string key, double score, std::string member) {
    return command{"ZADD", std::move(key), "INCR", format_real(score),
                   std::move(member)};
}

command zset_type::zrem(std::string key, std::vector<std::string> members) {
    return push_strings("ZREM", std::move(key), std::move(members));
}

command zset_type::zscore(std::string key, std::string member) {
    return command{"ZSCORE", std::move(key), std::move(member)};
}

command zset_type::zmscore(std::string key, std::vector<std::string> members) {
    return push_strings("ZMSCORE", std::move(key), std::move(members));
}

command zset_type::zcard(std::string key) { return command{"ZCARD", std::move(key)}; }

command zset_type::zcount(std::string key, std::string min, std::string max) {
    return command{"ZCOUNT", std::move(key), std::move(min), std::move(max)};
}

command zset_type::zincrby(std::string key, double amount, std::string member) {
    return command{"ZINCRBY", std::move(key), format_real(amount), std::move(member)};
}

command zset_type::zrange(std::string key, i64 start, i64 stop, bool with_scores) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("ZRANGE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::to_string(start));
    cmd.push_back(std::to_string(stop));
    if (with_scores) {
        cmd.push_back("WITHSCORES");
    }
    return cmd;
}

command zset_type::zrangebyscore(std::string key, std::string min, std::string max,
                                 bool with_scores, std::optional<i64> offset,
                                 std::optional<i64> count) {
    command cmd;
    cmd.reserve(9);
    cmd.push_back("ZRANGEBYSCORE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(min));
    cmd.push_back(std::move(max));
    if (with_scores) {
        cmd.push_back("WITHSCORES");
    }
    if (offset.has_value() || count.has_value()) {
        cmd.push_back("LIMIT");
        cmd.push_back(std::to_string(offset.value_or(0)));
        cmd.push_back(std::to_string(count.value_or(0)));
    }
    return cmd;
}

command zset_type::zrevrange(std::string key, i64 start, i64 stop,
                             bool with_scores) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("ZREVRANGE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::to_string(start));
    cmd.push_back(std::to_string(stop));
    if (with_scores) {
        cmd.push_back("WITHSCORES");
    }
    return cmd;
}

command zset_type::zrank(std::string key, std::string member) {
    return command{"ZRANK", std::move(key), std::move(member)};
}

command zset_type::zrevrank(std::string key, std::string member) {
    return command{"ZREVRANK", std::move(key), std::move(member)};
}

command zset_type::zremrangebyrank(std::string key, i64 start, i64 stop) {
    return command{"ZREMRANGEBYRANK", std::move(key), std::to_string(start),
                   std::to_string(stop)};
}

command zset_type::zremrangebyscore(std::string key, std::string min,
                                    std::string max) {
    return command{"ZREMRANGEBYSCORE", std::move(key), std::move(min),
                   std::move(max)};
}

command zset_type::zrandmember(std::string key, std::optional<i64> count) {
    command cmd;
    cmd.reserve(3);
    cmd.push_back("ZRANDMEMBER");
    cmd.push_back(std::move(key));
    if (count.has_value()) {
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

// --- stream -----------------------------------------------------------------

command stream_type::xadd(std::string key, std::string id, fields values) {
    command cmd;
    cmd.reserve(values.size() * 2 + 3);
    cmd.push_back("XADD");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(id));
    for (auto& field : values) {
        cmd.push_back(std::move(field.first));
        cmd.push_back(std::move(field.second));
    }
    return cmd;
}

command stream_type::xlen(std::string key) { return command{"XLEN", std::move(key)}; }

command stream_type::xdel(std::string key, std::vector<std::string> ids) {
    return push_strings("XDEL", std::move(key), std::move(ids));
}

command stream_type::xrange(std::string key, std::string start, std::string end,
                            std::optional<i64> count) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("XRANGE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(start));
    cmd.push_back(std::move(end));
    if (count.has_value()) {
        cmd.push_back("COUNT");
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command stream_type::xrevrange(std::string key, std::string end, std::string start,
                               std::optional<i64> count) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("XREVRANGE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(end));
    cmd.push_back(std::move(start));
    if (count.has_value()) {
        cmd.push_back("COUNT");
        cmd.push_back(std::to_string(*count));
    }
    return cmd;
}

command stream_type::xread(std::optional<i64> count, std::optional<i64> block,
                           std::vector<stream_spec> streams) {
    command cmd;
    cmd.reserve(streams.size() * 2 + 5);
    cmd.push_back("XREAD");
    if (count.has_value()) {
        cmd.push_back("COUNT");
        cmd.push_back(std::to_string(*count));
    }
    if (block.has_value()) {
        cmd.push_back("BLOCK");
        cmd.push_back(std::to_string(*block));
    }
    cmd.push_back("STREAMS");
    for (const stream_spec& spec : streams) {
        cmd.push_back(spec.key);
    }
    for (const stream_spec& spec : streams) {
        cmd.push_back(spec.last_id);
    }
    return cmd;
}

command stream_type::xgroup_create(std::string key, std::string group,
                                   std::string id, bool mkstream) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("XGROUP");
    cmd.push_back("CREATE");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(group));
    cmd.push_back(std::move(id));
    if (mkstream) {
        cmd.push_back("MKSTREAM");
    }
    return cmd;
}

command stream_type::xgroup_destroy(std::string key, std::string group) {
    return command{"XGROUP", "DESTROY", std::move(key), std::move(group)};
}

command stream_type::xgroup_setid(std::string key, std::string group,
                                  std::string id) {
    return command{"XGROUP", "SETID", std::move(key), std::move(group),
                   std::move(id)};
}

command stream_type::xack(std::string key, std::string group,
                          std::vector<std::string> ids) {
    command cmd;
    cmd.reserve(ids.size() + 3);
    cmd.push_back("XACK");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(group));
    for (std::string& id : ids) {
        cmd.push_back(std::move(id));
    }
    return cmd;
}

command stream_type::xpending(std::string key, std::string group) {
    return command{"XPENDING", std::move(key), std::move(group)};
}

// --- bitmap -----------------------------------------------------------------

command bitmap_type::setbit(std::string key, i64 offset, bool bit) {
    return command{"SETBIT", std::move(key), std::to_string(offset),
                   bit ? "1" : "0"};
}

command bitmap_type::getbit(std::string key, i64 offset) {
    return command{"GETBIT", std::move(key), std::to_string(offset)};
}

command bitmap_type::bitcount(std::string key, std::optional<i64> start,
                              std::optional<i64> end, bool bitmap_semantics) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("BITCOUNT");
    cmd.push_back(std::move(key));
    if (start.has_value()) {
        cmd.push_back(std::to_string(*start));
        if (end.has_value()) {
            cmd.push_back(std::to_string(*end));
        }
    }
    if (bitmap_semantics) {
        cmd.push_back("BIT");
    }
    return cmd;
}

command bitmap_type::bitpos(std::string key, bool bit, std::optional<i64> start,
                            std::optional<i64> end) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("BITPOS");
    cmd.push_back(std::move(key));
    cmd.push_back(bit ? "1" : "0");
    if (start.has_value()) {
        cmd.push_back(std::to_string(*start));
        if (end.has_value()) {
            cmd.push_back(std::to_string(*end));
        }
    }
    return cmd;
}

command bitmap_type::bitop(std::string op, std::string destination,
                           std::vector<std::string> keys) {
    command cmd;
    cmd.reserve(keys.size() + 3);
    cmd.push_back("BITOP");
    cmd.push_back(std::move(op));
    cmd.push_back(std::move(destination));
    for (std::string& key : keys) {
        cmd.push_back(std::move(key));
    }
    return cmd;
}

// --- HyperLogLog ------------------------------------------------------------

command hyperloglog_type::pfadd(std::string key,
                                std::optional<std::vector<std::string>> elements) {
    command cmd;
    cmd.reserve((elements.has_value() ? elements->size() : 0) + 2);
    cmd.push_back("PFADD");
    cmd.push_back(std::move(key));
    if (elements.has_value()) {
        for (std::string& element : *elements) {
            cmd.push_back(std::move(element));
        }
    }
    return cmd;
}

command hyperloglog_type::pfcount(std::vector<std::string> keys) {
    return push_keys("PFCOUNT", std::move(keys));
}

command hyperloglog_type::pfmerge(std::string destination,
                                  std::vector<std::string> sources) {
    return store("PFMERGE", std::move(destination), std::move(sources));
}

// --- geo --------------------------------------------------------------------

command geo_type::geoadd(std::string key, std::vector<point> points) {
    command cmd;
    cmd.reserve(points.size() * 3 + 2);
    cmd.push_back("GEOADD");
    cmd.push_back(std::move(key));
    for (const point& item : points) {
        cmd.push_back(format_real(item.longitude));
        cmd.push_back(format_real(item.latitude));
        cmd.push_back(item.member);
    }
    return cmd;
}

command geo_type::geopos(std::string key, std::vector<std::string> members) {
    return push_strings("GEOPOS", std::move(key), std::move(members));
}

command geo_type::geodist(std::string key, std::string member_a,
                          std::string member_b, std::optional<std::string> unit) {
    command cmd;
    cmd.reserve(6);
    cmd.push_back("GEODIST");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(member_a));
    cmd.push_back(std::move(member_b));
    if (unit.has_value()) {
        cmd.push_back(std::move(*unit));
    }
    return cmd;
}

command geo_type::geohash(std::string key, std::vector<std::string> members) {
    return push_strings("GEOHASH", std::move(key), std::move(members));
}

command geo_type::georadius(std::string key, double longitude, double latitude,
                            double radius, std::string unit, bool with_dist) {
    command cmd;
    cmd.reserve(8);
    cmd.push_back("GEORADIUS");
    cmd.push_back(std::move(key));
    cmd.push_back(format_real(longitude));
    cmd.push_back(format_real(latitude));
    cmd.push_back(format_real(radius));
    cmd.push_back(std::move(unit));
    if (with_dist) {
        cmd.push_back("WITHDIST");
    }
    return cmd;
}

command geo_type::georadiusbymember(std::string key, std::string member,
                                    double radius, std::string unit,
                                    bool with_dist) {
    command cmd;
    cmd.reserve(7);
    cmd.push_back("GEORADIUSBYMEMBER");
    cmd.push_back(std::move(key));
    cmd.push_back(std::move(member));
    cmd.push_back(format_real(radius));
    cmd.push_back(std::move(unit));
    if (with_dist) {
        cmd.push_back("WITHDIST");
    }
    return cmd;
}

// --- iterators --------------------------------------------------------------

string_iterator::string_iterator(const array_value& items) : items_(&items) {}

const std::string& string_iterator::operator*() const {
    return items_->at(index_).as_string();
}

const std::string* string_iterator::operator->() const {
    return &items_->at(index_).as_string();
}

string_iterator& string_iterator::operator++() {
    ++index_;
    return *this;
}

string_iterator string_iterator::operator++(int) {
    string_iterator copy = *this;
    ++index_;
    return copy;
}

bool string_iterator::operator==(const string_iterator& other) const noexcept {
    const bool this_end = items_ == nullptr || index_ >= items_->size();
    const bool other_end =
        other.items_ == nullptr || other.index_ >= other.items_->size();
    if (this_end || other_end) {
        return this_end && other_end;
    }
    return items_ == other.items_ && index_ == other.index_;
}

bool string_iterator::operator!=(const string_iterator& other) const noexcept {
    return !(*this == other);
}

zset_iterator::zset_iterator(const array_value& items) : items_(&items) {}

scored_member zset_iterator::operator*() const {
    scored_member member;
    member.member = decode_string(items_->at(index_));
    member.score = decode_real(items_->at(index_ + 1));
    return member;
}

zset_iterator& zset_iterator::operator++() {
    index_ += 2;
    return *this;
}

zset_iterator zset_iterator::operator++(int) {
    zset_iterator copy = *this;
    index_ += 2;
    return copy;
}

bool zset_iterator::operator==(const zset_iterator& other) const noexcept {
    const bool this_end = items_ == nullptr || index_ >= items_->size();
    const bool other_end =
        other.items_ == nullptr || other.index_ >= other.items_->size();
    if (this_end || other_end) {
        return this_end && other_end;
    }
    return items_ == other.items_ && index_ == other.index_;
}

bool zset_iterator::operator!=(const zset_iterator& other) const noexcept {
    return !(*this == other);
}

stream_iterator::stream_iterator(const array_value& items) : items_(&items) {}

redis_stream_entry stream_iterator::operator*() const {
    return parse_stream_entry(items_->at(index_));
}

stream_iterator& stream_iterator::operator++() {
    ++index_;
    return *this;
}

stream_iterator stream_iterator::operator++(int) {
    stream_iterator copy = *this;
    ++index_;
    return copy;
}

bool stream_iterator::operator==(const stream_iterator& other) const noexcept {
    const bool this_end = items_ == nullptr || index_ >= items_->size();
    const bool other_end =
        other.items_ == nullptr || other.index_ >= other.items_->size();
    if (this_end || other_end) {
        return this_end && other_end;
    }
    return items_ == other.items_ && index_ == other.index_;
}

bool stream_iterator::operator!=(const stream_iterator& other) const noexcept {
    return !(*this == other);
}

} // namespace redis
} // namespace crpc
