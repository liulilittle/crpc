// CRPC -- G19 step4 PR-8: Consul-compatible HTTP facade implementation.
//
// Design note: this file does NOT re-implement the registry's write path. It
// builds the same DTO frame `registry_client` puts on the wire and hands it to
// `registry_service::dispatch`, which runs the same handler the binary channel
// runs. That is the only way the two channels cannot drift: the ACL check, the
// epoch issue, the anchor water-mark, the log append and the idempotency map
// all live in one place.
//
// The consequence is that a request this layer cannot express as a legal DTO
// frame is refused here, before dispatch, with a 400 -- never forwarded as a
// half-filled frame that the handler would then reject for the wrong reason.
#include <crpc/cluster/consul_http.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/cluster/registry_service.hpp>
#include <crpc/cluster/registry_wire.hpp>
#include <crpc/crypto/base64.hpp>
#include <crpc/endian.hpp>
#include <crpc/http/message.hpp>

namespace crpc {
namespace cluster {
namespace {

// --- Small wire-frame builder ------------------------------------------------
//
// A request frame is: u16 dto_version, u32 total_len, then tags in ascending id
// order. The order matters: `decode_registry_tags` rejects a duplicate id, and
// a deterministic order makes two encodings of one request byte-identical (so a
// test can compare them).

void put_u16(std::vector<std::byte>& out, u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    endian::store_le16(out.data() + at, value);
}

void put_u32(std::vector<std::byte>& out, u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    endian::store_le32(out.data() + at, value);
}

void put_u64(std::vector<std::byte>& out, u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    endian::store_le64(out.data() + at, value);
}

// One tag: id, u32 length, payload.
void put_tag(std::vector<std::byte>& tags, u8 id,
             const std::vector<std::byte>& value) {
    tags.push_back(static_cast<std::byte>(id));
    put_u32(tags, static_cast<u32>(value.size()));
    tags.insert(tags.end(), value.begin(), value.end());
}

void put_string_tag(std::vector<std::byte>& tags, u8 id,
                    const std::string& value) {
    std::vector<std::byte> payload;
    put_u32(payload, static_cast<u32>(value.size()));
    payload.insert(payload.end(),
                   reinterpret_cast<const std::byte*>(value.data()),
                   reinterpret_cast<const std::byte*>(value.data()) +
                       value.size());
    put_tag(tags, id, payload);
}

void put_u64_tag(std::vector<std::byte>& tags, u8 id, u64 value) {
    std::vector<std::byte> payload;
    put_u64(payload, value);
    put_tag(tags, id, payload);
}

void put_u16_tag(std::vector<std::byte>& tags, u8 id, u16 value) {
    std::vector<std::byte> payload;
    put_u16(payload, value);
    put_tag(tags, id, payload);
}

void put_u8_tag(std::vector<std::byte>& tags, u8 id, u8 value) {
    put_tag(tags, id, std::vector<std::byte>{static_cast<std::byte>(value)});
}

// Wraps the tag region in the frame prefix. `tags` must already be in ascending
// id order.
[[nodiscard]] std::vector<std::byte> finish_frame(
    const std::vector<std::byte>& tags) {
    std::vector<std::byte> frame;
    put_u16(frame, registry_dto_version);
    put_u32(frame, static_cast<u32>(tags.size()));
    frame.insert(frame.end(), tags.begin(), tags.end());
    return frame;
}

// --- Response frame reader ---------------------------------------------------
//
// The response is the same tag frame shape. This reader is deliberately
// minimal: it walks the tags and hands back the ones the facade needs, and it
// refuses a frame whose prefix or tag lengths do not add up. It never guesses.

struct response_tag {
    u8 id = 0;
    span<const std::byte> value{};
};

[[nodiscard]] bool read_response_tags(span<const std::byte> frame,
                                      std::vector<response_tag>& out) {
    out.clear();
    if (frame.size() < registry_frame_prefix) {
        return false;
    }
    const u16 version = endian::load_le16(frame.data());
    if (version != registry_dto_version) {
        return false;
    }
    const u32 total = endian::load_le32(frame.data() + 2);
    if (total != frame.size() - registry_frame_prefix) {
        return false;
    }
    std::size_t at = registry_frame_prefix;
    while (at < frame.size()) {
        if (frame.size() - at < 5) {
            return false;
        }
        const u8 id = static_cast<u8>(frame[at]);
        const u32 len = endian::load_le32(frame.data() + at + 1);
        at += 5;
        if (len > frame.size() - at) {
            return false;
        }
        out.push_back(response_tag{id, frame.subspan(at, len)});
        at += len;
    }
    return true;
}

[[nodiscard]] const response_tag* find_tag(const std::vector<response_tag>& tags,
                                           u8 id) {
    for (const response_tag& tag : tags) {
        if (tag.id == id) {
            return &tag;
        }
    }
    return nullptr;
}

[[nodiscard]] bool read_u64_tag(const response_tag* tag, u64& out) {
    if (tag == nullptr || tag->value.size() != 8) {
        return false;
    }
    out = endian::load_le64(tag->value.data());
    return true;
}

// A boolean field on the response side is encoded as a u64 0/1 (see
// `encode_kv_get_response`: `scalar_tag_value_u64(value.found ? 1 : 0)`), not
// as a single byte. Reading it as a u8 would fail every time and silently turn
// every hit into a miss, so this reads the width the encoder actually writes.
[[nodiscard]] bool read_bool_tag(const response_tag* tag, bool& out) {
    u64 raw = 0;
    if (!read_u64_tag(tag, raw)) {
        return false;
    }
    out = raw != 0;
    return true;
}

// --- JSON helpers ------------------------------------------------------------
//
// The facade emits a small, fixed set of shapes. A full JSON DOM would be a
// dependency this layer does not need; the writer below escapes exactly what
// RFC 8259 requires and nothing else.

void append_json_string(std::string& out, std::string_view text) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        const unsigned char value = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (value < 0x20) {
                out += "\\u00";
                out.push_back(hex[(value >> 4) & 0x0Fu]);
                out.push_back(hex[value & 0x0Fu]);
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

// --- Request parsing helpers -------------------------------------------------

// The token's issue time, in the SYSTEM clock (Unix epoch milliseconds). The
// ACL window compares against the service's system clock, so this must be the
// same clock family; `steady_clock` has a per-process epoch and would make
// every request look either ancient or from the future.
[[nodiscard]] u64 now_millis() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// The token arrives in the `X-Consul-Token` header. A missing token is 401,
// decided here before any dispatch: `rpc_code` has no `unauthenticated` member,
// so the 401-vs-403 split cannot be made by the error mapping alone.
[[nodiscard]] std::string header_token(const http::request& request) {
    const std::string* value = request.headers.find("x-consul-token");
    return value != nullptr ? *value : std::string{};
}

// Splits `a=1&b=2` into pairs. Percent escapes are decoded; a malformed escape
// is a parse failure, not a silently dropped pair.
[[nodiscard]] bool parse_query(
    std::string_view query,
    std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    std::size_t at = 0;
    while (at <= query.size()) {
        const std::size_t amp = query.find('&', at);
        const std::string_view pair =
            amp == std::string_view::npos ? query.substr(at)
                                          : query.substr(at, amp - at);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            const std::string_view raw_key =
                eq == std::string_view::npos ? pair : pair.substr(0, eq);
            const std::string_view raw_value =
                eq == std::string_view::npos ? std::string_view{}
                                             : pair.substr(eq + 1);
            std::string key;
            std::string value;
            if (!http::percent_decode(raw_key, key).ok() ||
                !http::percent_decode(raw_value, value).ok()) {
                return false;
            }
            out.emplace_back(std::move(key), std::move(value));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        at = amp + 1;
    }
    return true;
}

[[nodiscard]] const std::string* query_value(
    const std::vector<std::pair<std::string, std::string>>& query,
    std::string_view name) {
    for (const auto& entry : query) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

// D-A-12: the documented query flags each endpoint accepts. Anything outside
// the endpoint's set is refused with 400 instead of being silently ignored.

[[nodiscard]] bool flag_is_known(const std::vector<std::pair<std::string, std::string>>& query,
                                 std::initializer_list<const char*> known) {
    for (const auto& entry : query) {
        bool known_flag = false;
        for (const char* name : known) {
            if (entry.first == name) {
                known_flag = true;
                break;
            }
        }
        if (!known_flag) {
            return false;
        }
    }
    return true;
}
// A strict unsigned parse: the whole string must be digits, and the value must
// fit. `"12x"`, `"-1"` and `""` are all failures.
[[nodiscard]] bool parse_u64(std::string_view text, u64& out) {
    if (text.empty()) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (value > (std::numeric_limits<u64>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// Consul durations: "5m", "30s", "100ms", "1h". A bare number is seconds.
[[nodiscard]] bool parse_duration(std::string_view text,
                                  std::chrono::milliseconds& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    u64 amount = 0;
    if (!parse_u64(text.substr(0, digits), amount)) {
        return false;
    }
    const std::string_view unit = text.substr(digits);
    u64 millis = 0;
    if (unit.empty() || unit == "s") {
        millis = amount * 1000u;
    } else if (unit == "ms") {
        millis = amount;
    } else if (unit == "m") {
        millis = amount * 60000u;
    } else if (unit == "h") {
        millis = amount * 3600000u;
    } else {
        return false;
    }
    out = std::chrono::milliseconds(static_cast<std::int64_t>(millis));
    return true;
}

// --- Consul service-definition parser ---------------------------------------
//
// A Consul registration body is a JSON object. This parser is deliberately
// narrow: it accepts a flat object whose members are strings, numbers, booleans
// or arrays of strings, and it extracts the six fields the registry can carry.
// Anything it does not understand is a parse failure, not a silently ignored
// member: a caller that sent `"Port": "80"` (a string) must be told, not given
// an instance on port 0.
//
// It is not a general JSON parser and does not pretend to be one. The facade
// only ever reads this one shape.

class json_reader {
public:
    explicit json_reader(std::string_view text) : text_(text) {}

    void skip_space() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool at_end() {
        skip_space();
        return at_ >= text_.size();
    }

    [[nodiscard]] bool consume(char expected) {
        skip_space();
        if (at_ >= text_.size() || text_[at_] != expected) {
            return false;
        }
        ++at_;
        return true;
    }

    // A JSON string with the standard escapes. A control character inside the
    // string is refused (RFC 8259 requires it be escaped).
    [[nodiscard]] bool read_string(std::string& out) {
        skip_space();
        if (at_ >= text_.size() || text_[at_] != '"') {
            return false;
        }
        ++at_;
        out.clear();
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                return false;
            }
            const char escape = text_[at_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                // A \u escape is decoded to UTF-8. A surrogate pair is handled
                // as one code point; a lone surrogate is refused.
                if (text_.size() - at_ < 4) {
                    return false;
                }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[at_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                        code |= static_cast<unsigned>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        code |= static_cast<unsigned>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        code |= static_cast<unsigned>(h - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (text_.size() - at_ < 6 || text_[at_] != '\\' ||
                        text_[at_ + 1] != 'u') {
                        return false;
                    }
                    at_ += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[at_++];
                        low <<= 4;
                        if (h >= '0' && h <= '9') {
                            low |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            low |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            low |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    // A JSON number, returned as its integer part. A fractional or exponent
    // form is refused: every numeric field this parser reads is an integer, and
    // accepting "80.5" as 80 would silently truncate.
    [[nodiscard]] bool read_unsigned(u64& out) {
        skip_space();
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            ++at_;
        }
        if (at_ == start) {
            return false;
        }
        if (at_ < text_.size() &&
            (text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E')) {
            return false;
        }
        return parse_u64(text_.substr(start, at_ - start), out);
    }

    [[nodiscard]] bool read_bool(bool& out) {
        skip_space();
        if (text_.substr(at_, 4) == "true") {
            at_ += 4;
            out = true;
            return true;
        }
        if (text_.substr(at_, 5) == "false") {
            at_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    // Skips one value of any shape. Used for members the facade does not read,
    // so an unknown member is tolerated (Consul clients send many) while a
    // malformed one is still an error.
    [[nodiscard]] bool skip_value() {
        skip_space();
        if (at_ >= text_.size()) {
            return false;
        }
        const char c = text_[at_];
        if (c == '"') {
            std::string ignored;
            return read_string(ignored);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++at_;
            skip_space();
            if (at_ < text_.size() && text_[at_] == close) {
                ++at_;
                return true;
            }
            while (true) {
                if (c == '{') {
                    std::string key;
                    if (!read_string(key) || !consume(':')) {
                        return false;
                    }
                }
                if (!skip_value()) {
                    return false;
                }
                skip_space();
                if (at_ >= text_.size()) {
                    return false;
                }
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_] == close) {
                    ++at_;
                    return true;
                }
                return false;
            }
        }
        if (c == 't' || c == 'f') {
            bool ignored = false;
            return read_bool(ignored);
        }
        if (c == 'n') {
            if (text_.substr(at_, 4) == "null") {
                at_ += 4;
                return true;
            }
            return false;
        }
        u64 ignored = 0;
        return read_unsigned(ignored);
    }

private:
    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

// Reads a Consul service definition. Returns false on any malformed input; the
// caller turns that into a 400.
[[nodiscard]] bool parse_register_body(std::string_view body, std::string& name,
                                       std::string& id, std::string& address,
                                       std::string& node, u64& port,
                                       bool& has_port,
                                       std::vector<std::string>& tags) {
    json_reader reader(body);
    if (!reader.consume('{')) {
        return false;
    }
    if (reader.consume('}')) {
        return true;
    }
    while (true) {
        std::string key;
        if (!reader.read_string(key) || !reader.consume(':')) {
            return false;
        }
        if (key == "Name") {
            if (!reader.read_string(name)) {
                return false;
            }
        } else if (key == "ID") {
            if (!reader.read_string(id)) {
                return false;
            }
        } else if (key == "Address") {
            if (!reader.read_string(address)) {
                return false;
            }
        } else if (key == "Node") {
            if (!reader.read_string(node)) {
                return false;
            }
        } else if (key == "Port") {
            if (!reader.read_unsigned(port)) {
                return false;
            }
            has_port = true;
        } else if (key == "Tags") {
            if (!reader.consume('[')) {
                return false;
            }
            if (!reader.consume(']')) {
                while (true) {
                    std::string tag;
                    if (!reader.read_string(tag)) {
                        return false;
                    }
                    tags.push_back(std::move(tag));
                    if (reader.consume(',')) {
                        continue;
                    }
                    if (reader.consume(']')) {
                        break;
                    }
                    return false;
                }
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            break;
        }
        return false;
    }
    return reader.at_end();
}

// --- Response writers --------------------------------------------------------

void set_index(http::response& response, u64 index) {
    response.headers.add("X-Consul-Index", std::to_string(index));
}

void write_json(http::response& response, int status, std::string body) {
    response.status_code = status;
    response.reason = mvc::reason_phrase(status);
    response.headers.add("Content-Type", "application/json");
    response.body = std::move(body);
}

// Consul's failure body is a bare string, not a JSON object. Keeping that shape
// means a stock Consul client parses it without a special case.
void write_failure(http::response& response, const consul_error& error,
                   u64 index) {
    response.status_code = error.http_status;
    response.reason = mvc::reason_phrase(error.http_status);
    response.headers.add("Content-Type", "text/plain; charset=utf-8");
    set_index(response, index);
    response.body = error.detail;
}

// Maps an rpc_error onto the Consul status code. The mapping is total: every
// code has an answer, and the default is 500 rather than a permissive 200.
//
// `rpc_code` has no `unauthenticated` / `already_exists` / `aborted` /
// `unimplemented` members (it is a transport-level enum, not gRPC's), so the
// facade maps onto what it has. The 401-vs-403 split is made where the token
// is inspected, before any dispatch, not here.
[[nodiscard]] int status_for(rpc_code code) {
    switch (code) {
    case rpc_code::ok:
        return 200;
    case rpc_code::invalid_argument:
    case rpc_code::decode_error:
        return 400;
    case rpc_code::permission_denied:
        return 403;
    case rpc_code::not_found:
    case rpc_code::no_such_method:
        return 404;
    case rpc_code::failed_precondition:
        return 409;
    case rpc_code::resource_exhausted:
    case rpc_code::overload:
        return 413;
    case rpc_code::unavailable:
    case rpc_code::shutting_down:
        return 503;
    case rpc_code::deadline_exceeded:
        return 504;
    default:
        return 500;
    }
}

// The reason token the registry attached, if any.
[[nodiscard]] std::string reason_of(const rpc_error& error) {
    const std::string* reason = error.metadata.find("registry.reason");
    return reason != nullptr ? *reason : std::string{};
}

// Allocates a fence token for the given service and node by dispatching
// `next_fence`.
//
// The registry refuses a register or deregister whose fence is zero
// (`registry_config::strict_fencing` defaults to true), and the HTTP channel
// has no client-side allocator, so the facade asks the service for one first.
// Failing here is the honest outcome: a registration that could not be fenced
// must not be applied, because the next writer would then hold a token that
// does not match the key's high-water mark.
[[nodiscard]] bool alloc_fence(registry_service& service,
                               const std::string& token,
                               const std::string& name,
                               const std::string& node, u64& fence) {
    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags, registry_tag_service, name);
    put_string_tag(tags, registry_tag_node_id, node);
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service.dispatch(
        registry_method_next_fence,
        span<const std::byte>(frame.data(), frame.size()), encoded);
    if (!dispatched.ok()) {
        return false;
    }
    std::vector<response_tag> tags_out;
    if (!read_response_tags(
            span<const std::byte>(encoded.data(), encoded.size()), tags_out)) {
        return false;
    }
    return read_u64_tag(find_tag(tags_out, registry_tag_fence), fence);
}

// The writer epoch the HTTP channel presents. It has no client-side epoch
// allocator, so it presents the service's own current epoch; the handler still
// issues a fresh one for the write, and this field only has to be non-zero to
// pass the "remote writers must present an epoch" gate. A freshly prepared
// service has `anchor_epoch == 0` (nothing has been written yet), so the floor
// is 1: zero is the sentinel the gate rejects, not a legal epoch.
[[nodiscard]] u64 writer_epoch_for(registry_service& service) {
    return std::max<u64>(1, service.anchor().authority_epoch);
}

} // namespace

consul_error to_consul_error(const rpc_error& error) {
    consul_error out;
    out.code = error.code;
    out.http_status = status_for(error.code);
    out.reason = reason_of(error);
    out.cas_conflict = out.reason == registry_reason_cas_conflict;
    // A cas conflict is the one 409 the facade produces on purpose; the body
    // names it so a client can tell it from a stale-anchor 409.
    if (out.cas_conflict) {
        out.http_status = 409;
        out.detail = "cas_conflict";
    } else if (!error.message.empty()) {
        out.detail = error.message;
    } else {
        out.detail = to_string(error.code);
    }
    return out;
}

consul_http::consul_http(registry_service& service, config options)
    : service_(service), config_(options) {}

// --- Route registration ------------------------------------------------------

namespace {

// The invoke trampoline. `route_entry::invoke` has no `this` of its own beyond
// `controller`, so each endpoint needs a distinct free function; they all
// forward to the facade method that owns the logic.
status invoke_catalog_services(void* controller, const http::request& request,
                               const mvc::route_params&, const codec&,
                               const codec_registry&, http::response& response) {
    static_cast<consul_http*>(controller)->handle_catalog_services(request,
                                                                  response);
    return status{};
}

status invoke_catalog_service(void* controller, const http::request& request,
                              const mvc::route_params& params, const codec&,
                              const codec_registry&, http::response& response) {
    const std::string* name = params.find("name");
    static_cast<consul_http*>(controller)->handle_catalog_service(
        request, name != nullptr ? *name : std::string{}, response);
    return status{};
}

status invoke_health_service(void* controller, const http::request& request,
                             const mvc::route_params& params, const codec&,
                             const codec_registry&, http::response& response) {
    const std::string* name = params.find("name");
    static_cast<consul_http*>(controller)->handle_health_service(
        request, name != nullptr ? *name : std::string{}, response);
    return status{};
}

status invoke_register(void* controller, const http::request& request,
                       const mvc::route_params&, const codec&,
                       const codec_registry&, http::response& response) {
    static_cast<consul_http*>(controller)->handle_register(request, response);
    return status{};
}

status invoke_deregister(void* controller, const http::request& request,
                         const mvc::route_params& params, const codec&,
                         const codec_registry&, http::response& response) {
    const std::string* id = params.find("id");
    static_cast<consul_http*>(controller)->handle_deregister(
        request, id != nullptr ? *id : std::string{}, response);
    return status{};
}

status invoke_kv_get(void* controller, const http::request& request,
                     const mvc::route_params& params, const codec&,
                     const codec_registry&, http::response& response) {
    const std::string* key = params.find("key");
    static_cast<consul_http*>(controller)->handle_kv(
        request, key != nullptr ? *key : std::string{}, consul_kv_op::get,
        response);
    return status{};
}

status invoke_kv_put(void* controller, const http::request& request,
                     const mvc::route_params& params, const codec&,
                     const codec_registry&, http::response& response) {
    const std::string* key = params.find("key");
    static_cast<consul_http*>(controller)->handle_kv(
        request, key != nullptr ? *key : std::string{}, consul_kv_op::put,
        response);
    return status{};
}

status invoke_kv_delete(void* controller, const http::request& request,
                        const mvc::route_params& params, const codec&,
                        const codec_registry&, http::response& response) {
    const std::string* key = params.find("key");
    static_cast<consul_http*>(controller)->handle_kv(
        request, key != nullptr ? *key : std::string{}, consul_kv_op::del,
        response);
    return status{};
}

status invoke_watch(void* controller, const http::request& request,
                    const mvc::route_params& params, const codec&,
                    const codec_registry&, http::response& response) {
    const std::string* key = params.find("key");
    static_cast<consul_http*>(controller)->handle_watch(
        request, key != nullptr ? *key : std::string{}, response);
    return status{};
}

[[nodiscard]] mvc::route_entry make_route(http::method verb, const char* path,
                                          const char* name,
                                          mvc::invoke_fn invoke,
                                          void* controller) {
    mvc::route_entry entry;
    entry.method = verb;
    entry.path = path;
    entry.name = name;
    entry.controller = controller;
    entry.invoke = invoke;
    return entry;
}

} // namespace

status consul_http::add_routes(mvc::router& routes) {
    // The KV key is a catch-all: Consul keys contain slashes and the whole tail
    // is the key. `*key` is the router's catch-all segment.
    const mvc::route_entry entries[] = {
        make_route(http::method::get, "/v1/catalog/services",
                   "catalog_services", &invoke_catalog_services, this),
        make_route(http::method::get, "/v1/catalog/service/:name",
                   "catalog_service", &invoke_catalog_service, this),
        make_route(http::method::get, "/v1/health/service/:name",
                   "health_service", &invoke_health_service, this),
        make_route(http::method::put, "/v1/agent/service/register",
                   "register", &invoke_register, this),
        make_route(http::method::put, "/v1/agent/service/deregister/:id",
                   "deregister", &invoke_deregister, this),
        make_route(http::method::get, "/v1/kv/*key", "kv_get", &invoke_kv_get,
                   this),
        make_route(http::method::put, "/v1/kv/*key", "kv_put", &invoke_kv_put,
                   this),
        make_route(http::method::del, "/v1/kv/*key", "kv_delete",
                   &invoke_kv_delete, this),
        make_route(http::method::get, "/v1/watch/kv/*key", "watch",
                   &invoke_watch, this),
    };
    for (const mvc::route_entry& entry : entries) {
        const status added = routes.add(entry);
        if (!added.ok()) {
            return added;
        }
    }
    return status{};
}

// --- Endpoint implementations ------------------------------------------------

void consul_http::handle_catalog_services(const http::request& request,
                                          http::response& response) {
    std::vector<std::pair<std::string, std::string>> query;
    if (!parse_query(request.query, query)) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "malformed query string", {}, false},
                      0);
        return;
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    // A catalog listing is a query with no service filter. The DTO requires a
    // token and nothing else, so the frame is one tag.
    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_query, span<const std::byte>(frame.data(), frame.size()),
        encoded);
    if (!dispatched.ok()) {
        const consul_error error = to_consul_error(dispatched.error());
        write_failure(response, error, service_.anchor().index);
        return;
    }

    // Decode with the library's own decoder rather than re-walking the tags:
    // the row block is a count-prefixed list inside ONE tag, and a hand-rolled
    // reader that treats the tag as a single row silently drops every row but
    // the first (or fails outright). One decoder, one shape.
    registry_query_response decoded;
    const status parsed = decode_query_response(
        span<const std::byte>(encoded.data(), encoded.size()), decoded);
    if (!parsed.ok()) {
        write_failure(response,
                      consul_error{rpc_code::internal, 500,
                                   "malformed registry response", {}, false},
                      service_.anchor().index);
        return;
    }
    const u64 index = decoded.anchor.index;

    std::vector<std::string> names;
    for (const registry_query_row& row : decoded.rows) {
        if (std::find(names.begin(), names.end(), row.instance.service) ==
            names.end()) {
            names.push_back(row.instance.service);
        }
    }
    std::sort(names.begin(), names.end());

    std::string body = "{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            body.push_back(',');
        }
        append_json_string(body, names[i]);
        body += ":[]";
    }
    body.push_back('}');
    write_json(response, 200, std::move(body));
    set_index(response, index);
}

void consul_http::handle_catalog_service(const http::request& request,
                                         const std::string& service,
                                         http::response& response) {
    if (service.empty()) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "missing service name", {}, false},
                      0);
        return;
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags, registry_tag_service, service);
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_query, span<const std::byte>(frame.data(), frame.size()),
        encoded);
    if (!dispatched.ok()) {
        write_failure(response, to_consul_error(dispatched.error()),
                      service_.anchor().index);
        return;
    }

    registry_query_response decoded;
    const status parsed = decode_query_response(
        span<const std::byte>(encoded.data(), encoded.size()), decoded);
    if (!parsed.ok()) {
        write_failure(response,
                      consul_error{rpc_code::internal, 500,
                                   "malformed registry response", {}, false},
                      service_.anchor().index);
        return;
    }
    const u64 index = decoded.anchor.index;

    std::string body = "[";
    bool first = true;
    for (const registry_query_row& row : decoded.rows) {
        const service_instance& instance = row.instance;
        if (!first) {
            body.push_back(',');
        }
        first = false;
        body += "{\"Node\":";
        append_json_string(body, instance.node_id);
        body += ",\"Address\":";
        append_json_string(body, instance.address.host);
        body += ",\"ServiceName\":";
        append_json_string(body, instance.service);
        body += ",\"ServicePort\":";
        body += std::to_string(instance.address.port);
        body += ",\"ServiceTags\":[";
        bool first_tag = true;
        for (const auto& entry : instance.meta) {
            if (!first_tag) {
                body.push_back(',');
            }
            first_tag = false;
            append_json_string(body, entry.second);
        }
        body += "],\"ServiceMeta\":{";
        bool first_meta = true;
        for (const auto& entry : instance.meta) {
            if (!first_meta) {
                body.push_back(',');
            }
            first_meta = false;
            append_json_string(body, entry.first);
            body.push_back(':');
            append_json_string(body, entry.second);
        }
        body += "},\"ServiceWeights\":{\"Passing\":";
        body += std::to_string(instance.weight);
        body += ",\"Warning\":";
        body += std::to_string(instance.weight);
        body += "},\"CreateIndex\":";
        body += std::to_string(instance.revision);
        body += ",\"ModifyIndex\":";
        body += std::to_string(instance.revision);
        body.push_back('}');
    }
    body.push_back(']');
    write_json(response, 200, std::move(body));
    set_index(response, index);
}

void consul_http::handle_health_service(const http::request& request,
                                        const std::string& service,
                                        http::response& response) {
    if (service.empty()) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "missing service name", {}, false},
                      0);
        return;
    }
    std::vector<std::pair<std::string, std::string>> query;
    if (!parse_query(request.query, query)) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "malformed query string", {}, false},
                      0);
        return;
    }
    // `passing=true` is the only accepted value; anything else is refused
    // rather than silently treated as false, because a caller that asked for
    // "passing=yes" and got every instance would believe the filter applied.
    bool passing_only = false;
    if (const std::string* passing = query_value(query, "passing")) {
        if (*passing == "true" || *passing == "1") {
            passing_only = true;
        } else if (*passing == "false" || *passing == "0") {
            passing_only = false;
        } else {
            write_failure(response,
                          consul_error{rpc_code::invalid_argument, 400,
                                       "passing must be true or false", {},
                                       false},
                          0);
            return;
        }
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags, registry_tag_service, service);
    if (passing_only) {
        put_u8_tag(tags, registry_tag_passing_only, 1);
    }
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_query, span<const std::byte>(frame.data(), frame.size()),
        encoded);
    if (!dispatched.ok()) {
        write_failure(response, to_consul_error(dispatched.error()),
                      service_.anchor().index);
        return;
    }

    registry_query_response decoded;
    const status parsed = decode_query_response(
        span<const std::byte>(encoded.data(), encoded.size()), decoded);
    if (!parsed.ok()) {
        write_failure(response,
                      consul_error{rpc_code::internal, 500,
                                   "malformed registry response", {}, false},
                      service_.anchor().index);
        return;
    }
    const u64 index = decoded.anchor.index;

    std::string body = "[";
    bool first = true;
    for (const registry_query_row& row : decoded.rows) {
        const service_instance& instance = row.instance;
        if (!first) {
            body.push_back(',');
        }
        first = false;
        body += "{\"Node\":{\"Node\":";
        append_json_string(body, instance.node_id);
        body += ",\"Address\":";
        append_json_string(body, instance.address.host);
        body += "},\"Service\":{\"ID\":";
        append_json_string(body, instance.node_id);
        body += ",\"Service\":";
        append_json_string(body, instance.service);
        body += ",\"Address\":";
        append_json_string(body, instance.address.host);
        body += ",\"Port\":";
        body += std::to_string(instance.address.port);
        body += "},\"Checks\":[{\"Status\":";
        append_json_string(body, to_string(instance.state));
        body += ",\"ServiceName\":";
        append_json_string(body, instance.service);
        body += "}]}";
    }
    body.push_back(']');
    write_json(response, 200, std::move(body));
    set_index(response, index);
}

void consul_http::handle_register(const http::request& request,
                                  http::response& response) {
    if (request.body.size() > config_.max_body_bytes) {
        write_failure(response,
                      consul_error{rpc_code::resource_exhausted, 413,
                                   "request body too large", {}, false},
                      0);
        return;
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    // The body is a Consul service definition. This layer reads exactly the
    // fields it can express as a registry instance and refuses the request when
    // a required one is absent. It does not accept a partial definition and
    // fill the rest with defaults: a registration with no port would create an
    // instance that can never be dialled.
    std::string name;
    std::string id;
    std::string address;
    std::string node;
    u64 port = 0;
    bool has_port = false;
    std::vector<std::string> tags;

    if (!parse_register_body(request.body, name, id, address, node, port,
                             has_port, tags)) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "malformed service definition", {}, false},
                      0);
        return;
    }
    if (name.empty() || !has_port) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "Name and Port are required", {}, false},
                      0);
        return;
    }
    // D-A-13: Port must be a valid TCP port 1..65535. Port 0 would create an
    // instance that can never be dialled; Port > 65535 is silently truncated by
    // the u16 cast otherwise.
    if (port < 1 || port > 65535) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "Port must be in 1..65535", {}, false},
                      0);
        return;
    }
    if (id.empty()) {
        id = name;
    }
    if (node.empty()) {
        node = id;
    }

    // The registry refuses a zero fence, so ask it for one first.
    u64 fence = 0;
    if (!alloc_fence(service_, token, name, node, fence)) {
        const rpc_error error = registry_failure(
            rpc_code::failed_precondition, "registry: fence allocation failed",
            "fence_unavailable");
        write_failure(response, to_consul_error(error),
                      service_.anchor().index);
        return;
    }

    // D-A-5: the incarnation must advance on every registration. The registry
    // rejects a register whose incarnation is not strictly above the stored one,
    // and a tombstone rejects every incarnation at or below its own, so a fixed
    // value of 1 makes a second registration (or a re-registration after a
    // deregister) impossible. Derive it from the current row: first registration
    // is 1, each later one is stored + 1.
    const u64 incarnation = service_.instance_incarnation(name, node) + 1;

    std::vector<std::byte> tags_wire;
    put_string_tag(tags_wire, registry_tag_token, token);
    put_u64_tag(tags_wire, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags_wire, registry_tag_service, name);
    put_string_tag(tags_wire, registry_tag_node_id, node);
    put_string_tag(tags_wire, registry_tag_host, address);
    put_u16_tag(tags_wire, registry_tag_port, static_cast<u16>(port));
    put_u64_tag(tags_wire, registry_tag_incarnation, incarnation);
    put_u64_tag(tags_wire, registry_tag_fence, fence);
    put_u64_tag(tags_wire, registry_tag_writer_epoch,
                writer_epoch_for(service_));
    // The idempotency key MUST name the method. Without it a register and a
    // deregister of the same (service, node, incarnation) collide on one key,
    // and the deregister is answered from the register's cached response --
    // the catalog is never touched and the instance survives. The binary
    // client appends the method for exactly this reason
    // (registry_client.cpp:326-336).
    //
    // D-A-5: it must ALSO name the incarnation. A key of `name + node +
    // "register"` alone is identical for every registration of the same
    // instance, so a second registration replays the FIRST request's cached
    // response: the port never moves and the catalog is never touched, while
    // the caller sees a 200. The incarnation is what distinguishes one
    // registration from the next.
    put_string_tag(tags_wire, registry_tag_client_request_id,
                   name + '\x1f' + node + '\x1f' + std::to_string(incarnation) +
                       '\x1f' + "register");
    // D-A-14: persist the Consul ID so a later deregister can find the row by
    // ID even when ID != Node. Stored in meta under the key "consul_id".
    if (!id.empty() && id != name) {
        metadata_list meta;
        meta.set("consul_id", id);
        std::vector<std::byte> encoded_meta;
        if (encode_meta_map(meta, encoded_meta).ok()) {
            put_tag(tags_wire, registry_tag_meta, encoded_meta);
        }
    } else if (!tags.empty()) {
        metadata_list meta;
        for (const std::string& tag : tags) {
            meta.set(tag, tag);
        }
        std::vector<std::byte> encoded_meta;
        if (encode_meta_map(meta, encoded_meta).ok()) {
            put_tag(tags_wire, registry_tag_meta, encoded_meta);
        }
    }
    const std::vector<std::byte> frame = finish_frame(tags_wire);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_register,
        span<const std::byte>(frame.data(), frame.size()), encoded);
    if (!dispatched.ok()) {
        write_failure(response, to_consul_error(dispatched.error()),
                      service_.anchor().index);
        return;
    }
    write_json(response, 200, "true");
    set_index(response, service_.anchor().index);
}

void consul_http::handle_deregister(const http::request& request,
                                    const std::string& instance_id,
                                    http::response& response) {
    if (instance_id.empty()) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "missing instance id", {}, false},
                      0);
        return;
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    // Consul's deregister path carries only the instance id, but the registry
    // keys an instance by (service, node_id). The id alone does not name a row,
    // so the catalog is consulted first: a caller that registered with
    // ID == Name (the default this facade applies) has service == node == id,
    // but a caller that chose a distinct ID has neither. Guessing would
    // deregister the wrong row or 404 a row that exists; the lookup cannot.
    //
    // D-A-14: the Consul ID is persisted in meta under "consul_id", so match
    // against meta first, then fall back to node_id for default-ID registrations.
    std::string service_name = instance_id;
    std::string node_name = instance_id;
    {
        std::vector<std::byte> query_tags;
        put_string_tag(query_tags, registry_tag_token, token);
        put_u64_tag(query_tags, registry_tag_token_issued_at, now_millis());
        const std::vector<std::byte> query_frame = finish_frame(query_tags);
        buffer query_response;
        if (service_
                .dispatch(registry_method_query,
                          span<const std::byte>(query_frame.data(),
                                                query_frame.size()),
                          query_response)
                .ok()) {
            registry_query_response catalog;
            if (decode_query_response(
                    span<const std::byte>(query_response.data(),
                                          query_response.size()),
                    catalog)
                    .ok()) {
                for (const registry_query_row& row : catalog.rows) {
                    const std::string* meta_id =
                        row.instance.meta.find("consul_id");
                    if (meta_id != nullptr && *meta_id == instance_id) {
                        service_name = row.instance.service;
                        node_name = row.instance.node_id;
                        break;
                    }
                    if (row.instance.node_id == instance_id) {
                        service_name = row.instance.service;
                        node_name = row.instance.node_id;
                        break;
                    }
                }
            }
        }
    }

    u64 fence = 0;
    if (!alloc_fence(service_, token, service_name, node_name, fence)) {
        const rpc_error error = registry_failure(
            rpc_code::failed_precondition, "registry: fence allocation failed",
            "fence_unavailable");
        write_failure(response, to_consul_error(error),
                      service_.anchor().index);
        return;
    }

    // D-A-5: the deregister must present the SAME incarnation the row holds.
    // The registry rejects a deregister whose incarnation is below the stored
    // one (`stale_incarnation`), so a fixed 1 fails on any row that was
    // registered more than once. Read the current value; a row that is already
    // gone keeps 0, which the registry treats as "no expectation".
    const u64 incarnation = service_.instance_incarnation(service_name, node_name);

    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags, registry_tag_service, service_name);
    put_string_tag(tags, registry_tag_node_id, node_name);
    put_u64_tag(tags, registry_tag_incarnation, incarnation);
    put_u64_tag(tags, registry_tag_fence, fence);
    put_u64_tag(tags, registry_tag_writer_epoch, writer_epoch_for(service_));
    // The idempotency key names the method: a deregister must not be answered
    // from a register's cached response (see `handle_register`). It also names
    // the incarnation, so a deregister of one incarnation is not confused with
    // a later one.
    put_string_tag(tags, registry_tag_client_request_id,
                   service_name + '\x1f' + node_name + '\x1f' +
                       std::to_string(incarnation) + '\x1f' + "deregister");
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_deregister,
        span<const std::byte>(frame.data(), frame.size()), encoded);
    if (!dispatched.ok()) {
        write_failure(response, to_consul_error(dispatched.error()),
                      service_.anchor().index);
        return;
    }
    write_json(response, 200, "true");
    set_index(response, service_.anchor().index);
}

void consul_http::handle_kv(const http::request& request,
                            const std::string& key, consul_kv_op op,
                            http::response& response) {
    if (key.empty()) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "missing key", {}, false},
                      0);
        return;
    }
    if (request.body.size() > config_.max_body_bytes) {
        write_failure(response,
                      consul_error{rpc_code::resource_exhausted, 413,
                                   "request body too large", {}, false},
                      0);
        return;
    }
    std::vector<std::pair<std::string, std::string>> query;
    if (!parse_query(request.query, query)) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "malformed query string", {}, false},
                      0);
        return;
    }
    if (!flag_is_known(query, {"cas", "index", "wait"})) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "unknown query flag", {}, false},
                      0);
        return;
    }
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    if (op == consul_kv_op::get) {
        std::vector<std::byte> tags;
        put_string_tag(tags, registry_tag_token, token);
        put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
        put_string_tag(tags, registry_tag_key, key);
        const std::vector<std::byte> frame = finish_frame(tags);

        buffer encoded;
        const status dispatched = service_.dispatch(
            registry_method_kv_get,
            span<const std::byte>(frame.data(), frame.size()), encoded);
        if (!dispatched.ok()) {
            write_failure(response, to_consul_error(dispatched.error()),
                          service_.anchor().index);
            return;
        }
        std::vector<response_tag> tags_out;
        if (!read_response_tags(
                span<const std::byte>(encoded.data(), encoded.size()),
                tags_out)) {
            write_failure(response,
                          consul_error{rpc_code::internal, 500,
                                       "malformed registry response", {},
                                       false},
                          service_.anchor().index);
            return;
        }
        u64 index = 0;
        (void)read_u64_tag(find_tag(tags_out, registry_tag_index), index);
        bool found = false;
        (void)read_bool_tag(find_tag(tags_out, registry_tag_found), found);
        if (!found) {
            // Consul answers a missing key with 404 and a null body, and still
            // sets the index so a watch can proceed.
            response.status_code = 404;
            response.reason = mvc::reason_phrase(404);
            response.headers.add("Content-Type", "application/json");
            set_index(response, index);
            response.body = "null";
            return;
        }
        const response_tag* value_tag = find_tag(tags_out, registry_tag_value);
        std::string raw;
        if (value_tag != nullptr) {
            raw.assign(reinterpret_cast<const char*>(value_tag->value.data()),
                       value_tag->value.size());
        }
        u64 create_index = 0;
        u64 modify_index = 0;
        (void)read_u64_tag(find_tag(tags_out, registry_tag_create_index),
                           create_index);
        (void)read_u64_tag(find_tag(tags_out, registry_tag_modify_index),
                           modify_index);

        std::string body = "[{\"Key\":";
        append_json_string(body, key);
        body += ",\"Value\":";
        append_json_string(body, crypto::base64_encode(raw));
        body += ",\"Flags\":0,\"CreateIndex\":";
        body += std::to_string(create_index);
        body += ",\"ModifyIndex\":";
        body += std::to_string(modify_index);
        body += ",\"LockIndex\":0,\"Session\":\"\"}]";
        write_json(response, 200, std::move(body));
        set_index(response, index);
        return;
    }

    if (op == consul_kv_op::put) {
        // The body is the raw value; Consul does not base64 it on the request
        // side. `?cas=N` selects compare-and-swap.
        if (request.body.size() > config_.max_value_bytes) {
            write_failure(response,
                          consul_error{rpc_code::resource_exhausted, 413,
                                       "value too large", {}, false},
                          0);
            return;
        }
        u64 cas = 0;
        bool has_cas = false;
        if (const std::string* raw_cas = query_value(query, "cas")) {
            if (!parse_u64(*raw_cas, cas)) {
                write_failure(response,
                              consul_error{rpc_code::invalid_argument, 400,
                                           "cas must be an unsigned integer",
                                           {}, false},
                              0);
                return;
            }
            has_cas = true;
        }

        std::vector<std::byte> tags;
        put_string_tag(tags, registry_tag_token, token);
        put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
        put_string_tag(tags, registry_tag_key, key);
        std::vector<std::byte> value(request.body.size());
        if (!request.body.empty()) {
            std::memcpy(value.data(), request.body.data(), request.body.size());
        }
        put_tag(tags, registry_tag_value, value);
        if (has_cas) {
            put_u64_tag(tags, registry_tag_cas, cas);
        }
        const std::vector<std::byte> frame = finish_frame(tags);

        buffer encoded;
        const status dispatched = service_.dispatch(
            has_cas ? registry_method_kv_cas : registry_method_kv_put,
            span<const std::byte>(frame.data(), frame.size()), encoded);
        if (!dispatched.ok()) {
            write_failure(response, to_consul_error(dispatched.error()),
                          service_.anchor().index);
            return;
        }
        write_json(response, 200, "true");
        set_index(response, service_.anchor().index);
        return;
    }

    // delete
    u64 cas = 0;
    bool has_cas = false;
    if (const std::string* raw_cas = query_value(query, "cas")) {
        if (!parse_u64(*raw_cas, cas)) {
            write_failure(response,
                          consul_error{rpc_code::invalid_argument, 400,
                                       "cas must be an unsigned integer", {},
                                       false},
                          0);
            return;
        }
        has_cas = true;
    }

    std::vector<std::byte> tags;
    put_string_tag(tags, registry_tag_token, token);
    put_u64_tag(tags, registry_tag_token_issued_at, now_millis());
    put_string_tag(tags, registry_tag_key, key);
    if (has_cas) {
        put_u64_tag(tags, registry_tag_cas, cas);
    }
    const std::vector<std::byte> frame = finish_frame(tags);

    buffer encoded;
    const status dispatched = service_.dispatch(
        registry_method_kv_delete,
        span<const std::byte>(frame.data(), frame.size()), encoded);
    if (!dispatched.ok()) {
        write_failure(response, to_consul_error(dispatched.error()),
                      service_.anchor().index);
        return;
    }
    write_json(response, 200, "true");
    set_index(response, service_.anchor().index);
}

void consul_http::handle_watch(const http::request& request,
                               const std::string& key,
                               http::response& response) {
    if (key.empty()) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "missing key", {}, false},
                      0);
        return;
    }
    std::vector<std::pair<std::string, std::string>> query;
    if (!parse_query(request.query, query)) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "malformed query string", {}, false},
                      0);
        return;
    }
    if (!flag_is_known(query, {"index", "wait"})) {
        write_failure(response,
                      consul_error{rpc_code::invalid_argument, 400,
                                   "unknown query flag", {}, false},
                      0);
        return;
    }
    u64 wait_index = 0;
    if (const std::string* raw_index = query_value(query, "index")) {
        if (!parse_u64(*raw_index, wait_index)) {
            write_failure(response,
                          consul_error{rpc_code::invalid_argument, 400,
                                       "index must be an unsigned integer", {},
                                       false},
                          0);
            return;
        }
    }
    std::chrono::milliseconds wait = config_.default_watch_wait;
    if (const std::string* raw_wait = query_value(query, "wait")) {
        if (!parse_duration(*raw_wait, wait)) {
            write_failure(response,
                          consul_error{rpc_code::invalid_argument, 400,
                                       "wait must be a duration", {}, false},
                          0);
            return;
        }
        if (wait > config_.max_watch_wait) {
            wait = config_.max_watch_wait;
        }
    }

    // D-A-11: authenticate before parking. An anonymous watch must be refused
    // with 401 immediately, not occupy a handler thread for the full wait.
    const std::string token = header_token(request);
    if (token.empty()) {
        write_failure(response,
                      consul_error{rpc_code::permission_denied, 401,
                                   "missing token", {}, false},
                      0);
        return;
    }

    // Long poll: park until the index moves past `wait_index`, the wait
    // expires, or the caller goes away. A timeout is 200 with the current value
    // and an unchanged index -- NOT 504. That is the whole point of the
    // endpoint: a client re-issues the same request and the loop continues.
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        const u64 current = service_.anchor().index;
        if (current > wait_index) {
            break;
        }
        std::this_thread::sleep_for(config_.watch_poll_interval);
    }

    // Answer with the current value, exactly as a GET would.
    handle_kv(request, key, consul_kv_op::get, response);
}

} // namespace cluster
} // namespace crpc
