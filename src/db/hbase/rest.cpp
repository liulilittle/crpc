// CRPC -- HBase REST adapter implementation.
#include <crpc/db/hbase/rest.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <crpc/crypto/base64.hpp>

namespace crpc {
namespace db {
namespace hbase {
namespace {

[[nodiscard]] std::string base64_of(span<const std::byte> data) {
    return crypto::base64_encode(data);
}

[[nodiscard]] std::string base64_of(std::string_view text) {
    return crypto::base64_encode(text);
}

[[nodiscard]] result<std::vector<std::byte>> base64_to_bytes(std::string_view text) {
    result<std::string> decoded = crypto::base64_decode(text);
    if (!decoded) {
        return decoded.error();
    }
    std::vector<std::byte> out(decoded.value().size());
    if (!out.empty()) {
        std::memcpy(out.data(), decoded.value().data(), out.size());
    }
    return out;
}

[[nodiscard]] std::string base64_to_text(std::string_view text) {
    result<std::string> decoded = crypto::base64_decode(text);
    return decoded ? decoded.value() : std::string{};
}

void put_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char c : value) {
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
        default:
            // Control characters must be escaped; everything else passes through, which is
            // what makes non-ASCII text survive unchanged.
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char digits[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0x0fu]);
                out.push_back(digits[static_cast<unsigned char>(c) & 0x0fu]);
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

// A strict reader for the CellSet shape. It is not a general JSON library: it accepts the
// subset the gateway produces and refuses everything else, so a reply that changes shape is
// an error rather than a silently empty result.
class json_reader {
public:
    explicit json_reader(std::string_view text) noexcept : text_(text) {}

    void skip_space() noexcept {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
                continue;
            }
            break;
        }
    }

    [[nodiscard]] bool at_end() noexcept {
        skip_space();
        return position_ >= text_.size();
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        skip_space();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool peek(char expected) noexcept {
        skip_space();
        return position_ < text_.size() && text_[position_] == expected;
    }

    [[nodiscard]] result<std::string> string() {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != '"') {
            return rpc_error{rpc_code::decode_error, "a JSON string was expected"};
        }
        ++position_;
        std::string out;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) {
                return rpc_error{rpc_code::decode_error, "a JSON escape is unterminated"};
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
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
                if (position_ + 4 > text_.size()) {
                    return rpc_error{rpc_code::decode_error, "a short \\u escape"};
                }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = text_[position_++];
                    code <<= 4;
                    if (digit >= '0' && digit <= '9') {
                        code |= static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        code |= static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        code |= static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        return rpc_error{rpc_code::decode_error, "a non-hex \\u escape"};
                    }
                }
                // UTF-8 encode; a lone surrogate becomes U+FFFD rather than invalid UTF-8.
                if (code >= 0xD800 && code <= 0xDFFF) {
                    out += "\xEF\xBF\xBD";
                } else if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return rpc_error{rpc_code::decode_error, "an unknown JSON escape"};
            }
        }
        return rpc_error{rpc_code::decode_error, "a JSON string is unterminated"};
    }

    [[nodiscard]] result<std::int64_t> integer() {
        skip_space();
        bool negative = false;
        if (position_ < text_.size() && text_[position_] == '-') {
            negative = true;
            ++position_;
        }
        if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
            return rpc_error{rpc_code::decode_error, "a JSON number was expected"};
        }
        std::int64_t value = 0;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            value = value * 10 + (text_[position_] - '0');
            ++position_;
        }
        return negative ? -value : value;
    }

    // Skips any value, for members this adapter does not model.
    [[nodiscard]] status skip_value() {
        skip_space();
        if (position_ >= text_.size()) {
            return status{rpc_code::decode_error, "a JSON value was expected"};
        }
        const char c = text_[position_];
        if (c == '"') {
            result<std::string> text = string();
            return text ? status{} : text.error();
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            const char open = c;
            ++position_;
            int depth = 1;
            while (position_ < text_.size() && depth > 0) {
                const char inner = text_[position_++];
                if (inner == '"') {
                    --position_;
                    result<std::string> text = string();
                    if (!text) {
                        return text.error();
                    }
                    continue;
                }
                if (inner == open) {
                    ++depth;
                } else if (inner == close) {
                    --depth;
                }
            }
            return status{};
        }
        // A number, true, false or null: consume the token.
        while (position_ < text_.size()) {
            const char inner = text_[position_];
            if (inner == ',' || inner == '}' || inner == ']' || inner == ' ' || inner == '\n' ||
                inner == '\r' || inner == '\t') {
                break;
            }
            ++position_;
        }
        return status{};
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
};

[[nodiscard]] status parse_cell(json_reader& reader, cell& out) {
    if (!reader.consume('{')) {
        return status{rpc_code::decode_error, "a cell must be a JSON object"};
    }
    if (reader.consume('}')) {
        return status{rpc_code::decode_error, "a cell has no column"};
    }
    for (;;) {
        result<std::string> key = reader.string();
        if (!key) {
            return key.error();
        }
        if (!reader.consume(':')) {
            return status{rpc_code::decode_error, "a JSON member needs a colon"};
        }
        if (key.value() == "column") {
            result<std::string> column = reader.string();
            if (!column) {
                return column.error();
            }
            // The column is base64 of "family" or "family:qualifier".
            const std::string decoded = base64_to_text(column.value());
            const std::size_t colon = decoded.find(':');
            if (colon == std::string::npos) {
                out.family = decoded;
            } else {
                out.family = decoded.substr(0, colon);
                out.qualifier = decoded.substr(colon + 1);
            }
        } else if (key.value() == "$") {
            result<std::string> value = reader.string();
            if (!value) {
                return value.error();
            }
            result<std::vector<std::byte>> bytes = base64_to_bytes(value.value());
            if (!bytes) {
                return bytes.error();
            }
            out.value = std::move(bytes.value());
        } else if (key.value() == "timestamp") {
            result<std::int64_t> stamp = reader.integer();
            if (!stamp) {
                return stamp.error();
            }
            out.timestamp = stamp.value();
        } else {
            const status skipped = reader.skip_value();
            if (!skipped) {
                return skipped;
            }
        }
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            return status{};
        }
        return status{rpc_code::decode_error, "a cell object is malformed"};
    }
}

[[nodiscard]] status parse_row(json_reader& reader, row& out) {
    if (!reader.consume('{')) {
        return status{rpc_code::decode_error, "a row must be a JSON object"};
    }
    bool saw_key = false;
    if (reader.consume('}')) {
        return status{rpc_code::decode_error, "a row has no key"};
    }
    for (;;) {
        result<std::string> key = reader.string();
        if (!key) {
            return key.error();
        }
        if (!reader.consume(':')) {
            return status{rpc_code::decode_error, "a JSON member needs a colon"};
        }
        if (key.value() == "key") {
            result<std::string> text = reader.string();
            if (!text) {
                return text.error();
            }
            result<std::vector<std::byte>> bytes = base64_to_bytes(text.value());
            if (!bytes) {
                return bytes.error();
            }
            out.key = std::move(bytes.value());
            saw_key = true;
        } else if (key.value() == "Cell") {
            if (!reader.consume('[')) {
                return status{rpc_code::decode_error, "Cell must be an array"};
            }
            if (reader.consume(']')) {
                // An empty cell list is legal: the row exists with no cells in the requested
                // columns.
            } else {
                for (;;) {
                    cell item;
                    const status parsed = parse_cell(reader, item);
                    if (!parsed) {
                        return parsed;
                    }
                    out.cells.push_back(std::move(item));
                    if (reader.consume(',')) {
                        continue;
                    }
                    if (reader.consume(']')) {
                        break;
                    }
                    return status{rpc_code::decode_error, "a Cell array is malformed"};
                }
            }
        } else {
            const status skipped = reader.skip_value();
            if (!skipped) {
                return skipped;
            }
        }
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            break;
        }
        return status{rpc_code::decode_error, "a row object is malformed"};
    }
    if (!saw_key) {
        return status{rpc_code::decode_error, "a row has no key"};
    }
    return status{};
}

[[nodiscard]] std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

} // namespace

result<std::string> encode_cell_set(span<const row> rows) {
    std::string out;
    out.reserve(64 + rows.size() * 64);
    out += "{\"Row\":[";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (r != 0) {
            out.push_back(',');
        }
        out += "{\"key\":";
        put_json_string(out, base64_of(span<const std::byte>(rows[r].key.data(),
                                                             rows[r].key.size())));
        out += ",\"Cell\":[";
        for (std::size_t c = 0; c < rows[r].cells.size(); ++c) {
            if (c != 0) {
                out.push_back(',');
            }
            const cell& item = rows[r].cells[c];
            std::string column = item.family;
            if (!item.qualifier.empty()) {
                column.push_back(':');
                column += item.qualifier;
            }
            out += "{\"column\":";
            put_json_string(out, base64_of(column));
            if (item.timestamp != 0) {
                out += ",\"timestamp\":";
                out += std::to_string(item.timestamp);
            }
            out += ",\"$\":";
            put_json_string(out, base64_of(span<const std::byte>(item.value.data(),
                                                                item.value.size())));
            out.push_back('}');
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

result<std::vector<row>> decode_cell_set(std::string_view json) {
    json_reader reader(json);
    if (!reader.consume('{')) {
        return rpc_error{rpc_code::decode_error, "a CellSet must be a JSON object"};
    }
    std::vector<row> out;
    if (reader.consume('}')) {
        return out;
    }
    for (;;) {
        result<std::string> key = reader.string();
        if (!key) {
            return key.error();
        }
        if (!reader.consume(':')) {
            return rpc_error{rpc_code::decode_error, "a JSON member needs a colon"};
        }
        if (key.value() == "Row") {
            if (!reader.consume('[')) {
                return rpc_error{rpc_code::decode_error, "Row must be an array"};
            }
            if (!reader.consume(']')) {
                for (;;) {
                    row item;
                    const status parsed = parse_row(reader, item);
                    if (!parsed) {
                        return parsed.error();
                    }
                    out.push_back(std::move(item));
                    if (reader.consume(',')) {
                        continue;
                    }
                    if (reader.consume(']')) {
                        break;
                    }
                    return rpc_error{rpc_code::decode_error, "a Row array is malformed"};
                }
            }
        } else {
            const status skipped = reader.skip_value();
            if (!skipped) {
                return skipped.error();
            }
        }
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            break;
        }
        return rpc_error{rpc_code::decode_error, "a CellSet object is malformed"};
    }
    if (!reader.at_end()) {
        return rpc_error{rpc_code::decode_error, "trailing bytes after a CellSet"};
    }
    return out;
}

std::string encode_path_segment(span<const std::byte> data) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size());
    for (const std::byte byte : data) {
        const auto value = static_cast<unsigned char>(byte);
        const bool unreserved = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                                (value >= '0' && value <= '9') || value == '-' || value == '_' ||
                                value == '.' || value == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(value));
        } else {
            out.push_back('%');
            out.push_back(digits[(value >> 4) & 0x0fu]);
            out.push_back(digits[value & 0x0fu]);
        }
    }
    return out;
}

rpc_code map_http_status(int status) noexcept {
    if (status >= 200 && status < 300) {
        return rpc_code::ok;
    }
    switch (status) {
    case 400:
        return rpc_code::invalid_argument;
    case 401:
    case 403:
        return rpc_code::failed_precondition;
    case 404:
        return rpc_code::not_found;
    case 409:
        return rpc_code::failed_precondition;
    case 410:
        return rpc_code::not_found;
    case 429:
        return rpc_code::overload;
    case 500:
        return rpc_code::internal;
    case 503:
        return rpc_code::unavailable;
    case 504:
        return rpc_code::deadline_exceeded;
    default:
        return rpc_code::application;
    }
}

client::client(client_options options, opener open)
    : options_(std::move(options)), open_(std::move(open)) {}

client::~client() = default;

result<std::unique_ptr<http::client>> client::open_link() {
    if (!open_) {
        return rpc_error{rpc_code::failed_precondition, "the HBase client has no transport"};
    }
    std::unique_ptr<http::client> link = open_();
    if (!link) {
        return rpc_error{rpc_code::failed_precondition,
                         "the HBase transport could not open a connection"};
    }
    return link;
}

result<http::response> client::call(http::method verb, std::string_view target,
                                    std::string_view body, std::string_view content_type) {
    result<std::unique_ptr<http::client>> link = open_link();
    if (!link) {
        return link.error();
    }
    http::header fields[2];
    std::size_t count = 0;
    fields[count].name = "Accept";
    fields[count].value = options_.accept;
    ++count;
    if (!content_type.empty()) {
        fields[count].name = "Content-Type";
        fields[count].value = std::string(content_type);
        ++count;
    }
    result<http::response> reply = link.value()->send(
        verb, target, span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()),
                                            body.size()),
        span<const http::header>(fields, count));
    if (!reply) {
        return reply.error();
    }
    return reply;
}

result<std::string> client::version() {
    result<http::response> reply = call(http::method::get, "/version");
    if (!reply) {
        return reply.error();
    }
    if (reply.value().status_code != 200) {
        return rpc_error{map_http_status(reply.value().status_code),
                         "the gateway refused the version request"};
    }
    return reply.value().body;
}

result<std::vector<std::string>> client::list_tables() {
    // The gateway answers `{"table":["name",...]}` for the root resource.
    result<http::response> reply = call(http::method::get, "/");
    if (!reply) {
        return reply.error();
    }
    if (reply.value().status_code != 200) {
        return rpc_error{map_http_status(reply.value().status_code),
                         "the gateway refused the table list"};
    }
    std::vector<std::string> out;
    json_reader reader(reply.value().body);
    if (!reader.consume('{')) {
        return rpc_error{rpc_code::decode_error, "a table list must be a JSON object"};
    }
    if (reader.consume('}')) {
        return out;
    }
    for (;;) {
        result<std::string> key = reader.string();
        if (!key) {
            return key.error();
        }
        if (!reader.consume(':')) {
            return rpc_error{rpc_code::decode_error, "a JSON member needs a colon"};
        }
        if (key.value() == "table") {
            if (!reader.consume('[')) {
                return rpc_error{rpc_code::decode_error, "table must be an array"};
            }
            if (!reader.consume(']')) {
                for (;;) {
                    // The gateway answers `{"table":[{"name":"t"}]}` — an object per table,
                    // not a bare string. A parser that expects a string fails with a decode
                    // error on a perfectly good reply, which is what happened first.
                    if (reader.peek('{')) {
                        if (!reader.consume('{')) {
                            return rpc_error{rpc_code::decode_error, "a table entry is malformed"};
                        }
                        std::string name;
                        bool saw_name = false;
                        if (!reader.consume('}')) {
                            for (;;) {
                                // Named `entry_key`: `key` is the outer member name, and
                                // shadowing it is a warning here.
                                result<std::string> entry_key = reader.string();
                                if (!entry_key) {
                                    return entry_key.error();
                                }
                                if (!reader.consume(':')) {
                                    return rpc_error{rpc_code::decode_error,
                                                    "a JSON member needs a colon"};
                                }
                                if (entry_key.value() == "name") {
                                    result<std::string> text = reader.string();
                                    if (!text) {
                                        return text.error();
                                    }
                                    name = std::move(text.value());
                                    saw_name = true;
                                } else {
                                    const status skipped = reader.skip_value();
                                    if (!skipped) {
                                        return skipped.error();
                                    }
                                }
                                if (reader.consume(',')) {
                                    continue;
                                }
                                if (reader.consume('}')) {
                                    break;
                                }
                                return rpc_error{rpc_code::decode_error,
                                                "a table entry is malformed"};
                            }
                        }
                        if (!saw_name) {
                            return rpc_error{rpc_code::decode_error,
                                             "a table entry carries no name"};
                        }
                        out.push_back(std::move(name));
                    } else {
                        // Older gateways send bare strings; accepting both costs one branch.
                        result<std::string> name = reader.string();
                        if (!name) {
                            return name.error();
                        }
                        out.push_back(std::move(name.value()));
                    }
                    if (reader.consume(',')) {
                        continue;
                    }
                    if (reader.consume(']')) {
                        break;
                    }
                    return rpc_error{rpc_code::decode_error, "a table array is malformed"};
                }
            }
        } else {
            const status skipped = reader.skip_value();
            if (!skipped) {
                return skipped.error();
            }
        }
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            break;
        }
        return rpc_error{rpc_code::decode_error, "a table list object is malformed"};
    }
    return out;
}

result<bool> client::table_exists(std::string_view table) {
    result<std::vector<std::string>> tables = list_tables();
    if (!tables) {
        return tables.error();
    }
    for (const std::string& name : tables.value()) {
        if (name == table) {
            return true;
        }
    }
    return false;
}

status client::wait_for_table(std::string_view table, bool present) {
    if (options_.transition_timeout_ms == 0) {
        return status{};
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options_.transition_timeout_ms);
    for (;;) {
        result<bool> exists = table_exists(table);
        if (!exists) {
            return exists.error();
        }
        if (exists.value() == present) {
            return status{};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return status{rpc_code::deadline_exceeded,
                          present ? "the table did not appear in time"
                                  : "the table did not disappear in time"};
        }
        // The gateway exposes no completion signal for a table transition, so this polls.
        // It runs on the calling thread; a caller that cannot block should set
        // `transition_timeout_ms = 0` and poll `table_exists` itself.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

status client::create_table(std::string_view table, span<const std::string> families) {
    if (families.empty()) {
        return status{rpc_code::invalid_argument, "a table needs at least one column family"};
    }
    // The gateway's schema document is XML; a JSON schema is also accepted by newer
    // gateways, but XML is what this server version documented and accepted.
    std::string body;
    body.reserve(64 + families.size() * 40);
    body += "<TableSchema name=\"";
    body += xml_escape(table);
    body += "\">";
    for (const std::string& family : families) {
        body += "<ColumnSchema name=\"";
        body += xml_escape(family);
        body += "\"/>";
    }
    body += "</TableSchema>";
    std::string target("/");
    target += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));
    target += "/schema";
    result<http::response> reply = call(http::method::put, target, body, "text/xml");
    if (!reply) {
        return reply.error();
    }
    const int code = reply.value().status_code;
    if (code != 200 && code != 201) {
        return status{map_http_status(code),
                      "the gateway refused to create the table: " + reply.value().body};
    }
    // The reply means the request was accepted, not that the table is usable: a write issued
    // before the table is enabled fails with a table-state error.
    return wait_for_table(table, true);
}

status client::delete_table(std::string_view table) {
    std::string target("/");
    target += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));
    target += "/schema";
    result<http::response> reply = call(http::method::del, target);
    if (!reply) {
        return reply.error();
    }
    const int code = reply.value().status_code;
    // Deleting a table that does not exist is not an error for a caller that wants it gone.
    if (code == 404) {
        return status{};
    }
    if (code != 200) {
        return status{map_http_status(code), "the gateway refused to delete the table"};
    }
    // HBase deletes asynchronously. Returning before the table is gone makes the obvious
    // next call — create it again — fail against a table that is still being removed.
    return wait_for_table(table, false);
}

status client::put_row(std::string_view table, const row& value) {
    std::string target("/");
    target += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));
    // `/false` means "do not check whether the row already exists".
    target += "/false";
    result<std::string> body = encode_cell_set(span<const row>(&value, 1));
    if (!body) {
        return body.error();
    }
    result<http::response> reply =
        call(http::method::put, target, body.value(), "application/json");
    if (!reply) {
        return reply.error();
    }
    const int code = reply.value().status_code;
    if (code != 200 && code != 201) {
        return status{map_http_status(code),
                      "the gateway refused the row: " + reply.value().body};
    }
    return status{};
}

result<row> client::get_row(std::string_view table, span<const std::byte> key) {
    std::string target("/");
    target += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));
    target += "/";
    // The path carries the *raw* key, percent-encoded. Base64 here is the mistake that
    // produces 404s for rows that exist: the gateway decodes the path as the key itself.
    target += encode_path_segment(key);
    result<http::response> reply = call(http::method::get, target);
    if (!reply) {
        return reply.error();
    }
    const int code = reply.value().status_code;
    if (code == 404) {
        return rpc_error{rpc_code::not_found, "the row does not exist"};
    }
    if (code != 200) {
        return rpc_error{map_http_status(code),
                         "the gateway refused the row read: " + reply.value().body};
    }
    result<std::vector<row>> rows = decode_cell_set(reply.value().body);
    if (!rows) {
        return rows.error();
    }
    if (rows.value().empty()) {
        return rpc_error{rpc_code::not_found, "the gateway returned no row"};
    }
    return std::move(rows.value().front());
}

status client::delete_row(std::string_view table, span<const std::byte> key) {
    std::string target("/");
    target += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));
    target += "/";
    target += encode_path_segment(key);
    result<http::response> reply = call(http::method::del, target);
    if (!reply) {
        return reply.error();
    }
    const int code = reply.value().status_code;
    if (code == 200 || code == 404) {
        return status{};
    }
    return status{map_http_status(code), "the gateway refused to delete the row"};
}

result<std::vector<row>> client::scan(std::string_view table, span<const std::byte> start,
                                      std::size_t limit) {
    std::string table_path("/");
    table_path += encode_path_segment(span<const std::byte>(
        reinterpret_cast<const std::byte*>(table.data()), table.size()));

    // A scanner is opened with a filter document, then drained with GETs. The batch size
    // bounds each reply, which is what keeps a large table from arriving in one message.
    std::string request;
    request += "{\"batch\":";
    request += std::to_string(limit == 0 ? 1000 : limit);
    if (!start.empty()) {
        request += ",\"startRow\":";
        put_json_string(request, base64_of(start));
    }
    request += "}";
    result<http::response> opened =
        call(http::method::put, table_path + "/scanner", request, "application/json");
    if (!opened) {
        return opened.error();
    }
    if (opened.value().status_code != 201 && opened.value().status_code != 200) {
        return rpc_error{map_http_status(opened.value().status_code),
                         "the gateway refused to open a scanner: " + opened.value().body};
    }
    // The scanner's location is the only handle to it; without it the rows cannot be read and
    // the scanner would leak until it timed out.
    const std::string* location = opened.value().headers.find("location");
    if (location == nullptr || location->empty()) {
        return rpc_error{rpc_code::protocol_error,
                         "a scanner reply carries no Location header"};
    }
    std::string target = *location;
    // A Location may be absolute; this client speaks to one host, so the authority is
    // dropped rather than trusted (a redirect to another host would send the request there).
    const std::size_t scheme = target.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = target.find('/', scheme + 3);
        target = slash == std::string::npos ? std::string("/") : target.substr(slash);
    }

    std::vector<row> out;
    bool released = false;
    for (;;) {
        result<http::response> batch = call(http::method::get, target);
        if (!batch) {
            break;
        }
        const int code = batch.value().status_code;
        if (code == 204) {
            // No more rows.
            released = true;
            break;
        }
        if (code != 200) {
            (void)call(http::method::del, target);
            return rpc_error{map_http_status(code),
                             "the gateway refused a scanner read: " + batch.value().body};
        }
        result<std::vector<row>> rows = decode_cell_set(batch.value().body);
        if (!rows) {
            (void)call(http::method::del, target);
            return rows.error();
        }
        if (rows.value().empty()) {
            released = true;
            break;
        }
        for (row& item : rows.value()) {
            out.push_back(std::move(item));
            if (limit != 0 && out.size() >= limit) {
                break;
            }
        }
        if (limit != 0 && out.size() >= limit) {
            break;
        }
    }
    // A scanner that was not drained must be deleted: an abandoned one holds server resources
    // until it times out, and the gateway keeps it in memory.
    if (!released) {
        (void)call(http::method::del, target);
    }
    return out;
}

} // namespace hbase
} // namespace db
} // namespace crpc