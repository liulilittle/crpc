#include <crpc/http/multipart.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace crpc::http {
namespace {

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool is_token_char(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool is_header_token(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!is_token_char(c)) {
            return false;
        }
    }
    return true;
}

bool has_control(std::string_view text) noexcept {
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7fu) {
            return true;
        }
    }
    return false;
}

struct parameter {
    std::string name;
    std::string value;
};

status parse_parameters(std::string_view text, std::vector<parameter>& out,
                        std::size_t max_value_bytes, std::size_t max_count) {
    out.clear();
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() &&
               (text[pos] == ';' || text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }
        if (pos >= text.size()) {
            break;
        }
        if (out.size() >= max_count) {
            return rpc_error(rpc_code::resource_exhausted,
                                    "too many multipart parameters");
        }
        const std::size_t name_begin = pos;
        while (pos < text.size() && is_token_char(text[pos])) {
            ++pos;
        }
        std::string_view name = text.substr(name_begin, pos - name_begin);
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }
        if (name.empty() || pos >= text.size() || text[pos] != '=') {
            return rpc_error(rpc_code::protocol_error,
                                    "malformed multipart parameter");
        }
        ++pos;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }
        std::string value;
        if (pos < text.size() && text[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < text.size()) {
                const char c = text[pos++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (pos >= text.size()) {
                        return rpc_error(rpc_code::protocol_error,
                                                "unterminated quoted parameter");
                    }
                    value.push_back(text[pos++]);
                    continue;
                }
                if (static_cast<unsigned char>(c) < 0x20u || c == 0x7f) {
                    return rpc_error(rpc_code::protocol_error,
                                            "control character in parameter");
                }
                value.push_back(c);
            }
            if (!closed) {
                return rpc_error(rpc_code::protocol_error,
                                        "unterminated quoted parameter");
            }
        } else {
            const std::size_t value_begin = pos;
            while (pos < text.size() && text[pos] != ';') {
                ++pos;
            }
            const std::string_view raw =
                trim_ows(text.substr(value_begin, pos - value_begin));
            if (raw.find('"') != std::string_view::npos) {
                return rpc_error(rpc_code::protocol_error,
                                        "malformed multipart parameter");
            }
            value.assign(raw.data(), raw.size());
        }
        if (value.size() > max_value_bytes) {
            return rpc_error(rpc_code::resource_exhausted,
                                    "multipart parameter exceeds limit");
        }
        out.push_back(parameter{std::string(name.data(), name.size()),
                                std::move(value)});
    }
    return status{};
}

bool valid_boundary_char(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u > 0x7eu || c == '"') {
        return false;
    }
    return true;
}

const parameter* find_parameter(const std::vector<parameter>& params,
                                std::string_view name) noexcept {
    for (const parameter& p : params) {
        if (ascii_iequals(p.name, name)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

const multipart_limits& default_multipart_limits() noexcept {
    static const multipart_limits limits{};
    return limits;
}

result<std::string> sanitize_filename(std::string_view raw) {
    if (raw.empty()) {
        return rpc_error(rpc_code::invalid_argument,
                                "empty upload filename");
    }
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7fu) {
            return rpc_error(rpc_code::invalid_argument,
                                    "control character in upload filename");
        }
    }
    // Treat both separators and a Windows drive colon as directory components.
    std::size_t base = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '/' || raw[i] == '\\' || raw[i] == ':') {
            base = i + 1;
        }
    }
    std::string_view name = raw.substr(base);
    // Windows silently strips trailing spaces and dots; do it explicitly so
    // the sanitized name cannot become a device alias like "con.".
    std::size_t end = name.size();
    while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == '.')) {
        --end;
    }
    name = name.substr(0, end);
    if (name.empty() || name == "." || name == "..") {
        return rpc_error(rpc_code::invalid_argument,
                                "unsafe upload filename");
    }
    return std::string(name.data(), name.size());
}

result<std::string> multipart_boundary(std::string_view content_type,
                                       std::size_t max_bytes) {
    const std::size_t semicolon = content_type.find(';');
    const std::string_view media =
        trim_ows(semicolon == std::string_view::npos
                     ? content_type
                     : content_type.substr(0, semicolon));
    if (!ascii_iequals(media, "multipart/form-data")) {
        return rpc_error(rpc_code::protocol_error,
                                "content type is not multipart/form-data");
    }
    std::vector<parameter> params;
    const std::string_view rest =
        semicolon == std::string_view::npos
            ? std::string_view{}
            : content_type.substr(semicolon);
    status parsed = parse_parameters(rest, params, 4096, 16);
    if (!parsed.ok()) {
        return parsed.error();
    }
    const parameter* boundary = find_parameter(params, "boundary");
    if (boundary == nullptr) {
        return rpc_error(rpc_code::protocol_error,
                                "multipart body has no boundary");
    }
    if (boundary->value.empty() || boundary->value.size() > max_bytes) {
        return rpc_error(rpc_code::protocol_error,
                                "multipart boundary has invalid length");
    }
    for (char c : boundary->value) {
        if (!valid_boundary_char(c)) {
            return rpc_error(rpc_code::protocol_error,
                                    "multipart boundary has invalid character");
        }
    }
    if (boundary->value.back() == ' ') {
        return rpc_error(rpc_code::protocol_error,
                                "multipart boundary ends with a space");
    }
    return boundary->value;
}

multipart_parser::multipart_parser(std::string boundary,
                                   multipart_handler& handler,
                                   multipart_limits limits)
    : limits_(limits),
      boundary_(std::move(boundary)),
      handler_(&handler) {
    if (boundary_.empty()) {
        (void)fail(rpc_code::protocol_error, "empty multipart boundary");
        return;
    }
    body_delimiter_.reserve(boundary_.size() + 4);
    body_delimiter_.append("\r\n--");
    body_delimiter_.append(boundary_);
    start_delimiter_.reserve(boundary_.size() + 2);
    start_delimiter_.append("--");
    start_delimiter_.append(boundary_);
}

result<multipart_parser> multipart_parser::create(
    std::string_view content_type, multipart_handler& handler,
    multipart_limits limits) {
    result<std::string> boundary =
        multipart_boundary(content_type, limits.max_boundary_bytes);
    if (!boundary.ok()) {
        return boundary.error();
    }
    return multipart_parser(boundary.value(), handler, limits);
}

status multipart_parser::fail(rpc_code code, std::string message) {
    state_ = state::failed;
    error_ = rpc_error(code, std::move(message));
    return status{error_};
}

status multipart_parser::feed(span<const std::byte> data) {
    if (state_ == state::failed) {
        return status{error_};
    }
    if (state_ == state::done) {
        // Anything after the closing delimiter is the epilogue and is ignored.
        return status{};
    }
    if (!data.empty()) {
        buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
    status result = run();
    compact();
    return result;
}

status multipart_parser::finish() {
    if (state_ == state::failed) {
        return status{error_};
    }
    if (state_ == state::done) {
        return status{};
    }
    return fail(rpc_code::protocol_error, "truncated multipart body");
}

void multipart_parser::compact() {
    if (state_ == state::done) {
        buffer_.clear();
        pos_ = 0;
        return;
    }
    if (pos_ == 0) {
        return;
    }
    if (pos_ >= buffer_.size()) {
        buffer_.clear();
    } else {
        buffer_.erase(0, pos_);
    }
    pos_ = 0;
}

status multipart_parser::emit_data(std::size_t begin, std::size_t end) {
    if (end <= begin) {
        return status{};
    }
    const std::size_t count = end - begin;
    if (count > limits_.max_part_bytes ||
        current_part_bytes_ > limits_.max_part_bytes - count) {
        return fail(rpc_code::resource_exhausted,
                    "multipart part exceeds max_part_bytes");
    }
    if (count > limits_.max_total_bytes ||
        total_bytes_ > limits_.max_total_bytes - count) {
        return fail(rpc_code::resource_exhausted,
                    "multipart body exceeds max_total_bytes");
    }
    current_part_bytes_ += count;
    total_bytes_ += count;
    const span<const std::byte> view(
        reinterpret_cast<const std::byte*>(buffer_.data() + begin), count);
    status result = handler_->on_part_data(view);
    if (!result.ok()) {
        return fail(result.code(), result.message());
    }
    return status{};
}

status multipart_parser::begin_part() {
    if (part_count_ >= limits_.max_parts) {
        return fail(rpc_code::resource_exhausted,
                    "multipart body exceeds max_parts");
    }
    current_ = multipart_part_info{};
    current_.index = static_cast<std::uint64_t>(part_count_);
    ++part_count_;
    current_part_bytes_ = 0;
    state_ = state::part_header;
    return status{};
}

status multipart_parser::end_part() {
    status result = handler_->on_part_end();
    if (!result.ok()) {
        return fail(result.code(), result.message());
    }
    current_part_bytes_ = 0;
    return status{};
}

status multipart_parser::parse_part_headers(std::string_view block) {
    std::size_t pos = 0;
    std::size_t fields = 0;
    bool saw_disposition = false;
    std::string_view disposition;

    while (pos < block.size()) {
        const std::size_t line_end = block.find("\r\n", pos);
        const std::string_view line =
            line_end == std::string_view::npos
                ? block.substr(pos)
                : block.substr(pos, line_end - pos);
        pos = line_end == std::string_view::npos ? block.size() : line_end + 2;

        if (line.empty()) {
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return fail(rpc_code::protocol_error,
                        "multipart obs-fold is not allowed");
        }
        if (line.find('\r') != std::string_view::npos ||
            line.find('\n') != std::string_view::npos) {
            return fail(rpc_code::protocol_error,
                        "bare CR or LF in multipart header");
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return fail(rpc_code::protocol_error,
                        "multipart header has no colon");
        }
        const std::string_view name = trim_ows(line.substr(0, colon));
        const std::string_view value = trim_ows(line.substr(colon + 1));
        if (!is_header_token(name)) {
            return fail(rpc_code::protocol_error,
                        "invalid multipart header name");
        }
        if (has_control(value)) {
            return fail(rpc_code::protocol_error,
                        "control character in multipart header value");
        }
        if (fields >= limits_.max_header_fields) {
            return fail(rpc_code::resource_exhausted,
                        "multipart part has too many headers");
        }
        ++fields;

        if (ascii_iequals(name, "content-disposition")) {
            if (saw_disposition) {
                return fail(rpc_code::protocol_error,
                            "duplicate Content-Disposition");
            }
            saw_disposition = true;
            disposition = value;
        } else if (ascii_iequals(name, "content-type")) {
            if (current_.content_type.empty()) {
                current_.content_type.assign(value.data(), value.size());
            }
        }
        current_.headers.add(name, value);
    }

    if (!saw_disposition) {
        return fail(rpc_code::protocol_error,
                    "multipart part has no Content-Disposition");
    }

    const std::size_t semicolon = disposition.find(';');
    const std::string_view kind =
        trim_ows(semicolon == std::string_view::npos
                     ? disposition
                     : disposition.substr(0, semicolon));
    if (!ascii_iequals(kind, "form-data")) {
        return fail(rpc_code::protocol_error,
                    "multipart part disposition is not form-data");
    }

    std::vector<parameter> params;
    const std::string_view rest =
        semicolon == std::string_view::npos
            ? std::string_view{}
            : disposition.substr(semicolon);
    status parsed = parse_parameters(rest, params,
                                     limits_.max_parameter_bytes, 8);
    if (!parsed.ok()) {
        return fail(parsed.code(), parsed.message());
    }
    const parameter* name = find_parameter(params, "name");
    if (name == nullptr || name->value.empty()) {
        return fail(rpc_code::protocol_error,
                    "multipart part has no form field name");
    }
    current_.name = name->value;

    const parameter* filename = find_parameter(params, "filename");
    if (filename != nullptr && !filename->value.empty()) {
        result<std::string> safe = sanitize_filename(filename->value);
        if (!safe.ok()) {
            return fail(rpc_code::protocol_error,
                        "unsafe multipart filename: " + safe.error().message);
        }
        current_.filename = std::move(safe.value());
        current_.has_filename = true;
    }
    return status{};
}

status multipart_parser::run() {
    for (;;) {
        switch (state_) {
        case state::preamble: {
            std::size_t found = buffer_.find(start_delimiter_, pos_);
            while (found != std::string::npos) {
                const bool line_start =
                    found == 0 || buffer_[found - 1] == '\n';
                if (line_start) {
                    break;
                }
                found = buffer_.find(start_delimiter_, found + 1);
            }
            if (found == std::string::npos) {
                const std::size_t keep = start_delimiter_.size() + 2;
                if (buffer_.size() - pos_ > keep) {
                    const std::size_t discard = buffer_.size() - pos_ - keep;
                    if (discard > limits_.max_preamble_bytes ||
                        preamble_bytes_ > limits_.max_preamble_bytes - discard) {
                        return fail(rpc_code::resource_exhausted,
                                    "multipart preamble exceeds limit");
                    }
                    preamble_bytes_ += discard;
                    pos_ += discard;
                }
                return status{};
            }
            const std::size_t skipped = found - pos_;
            if (skipped > limits_.max_preamble_bytes ||
                preamble_bytes_ > limits_.max_preamble_bytes - skipped) {
                return fail(rpc_code::resource_exhausted,
                            "multipart preamble exceeds limit");
            }
            preamble_bytes_ += skipped;
            pos_ = found + start_delimiter_.size();
            state_ = state::after_boundary;
            break;
        }
        case state::after_boundary: {
            if (buffer_.size() - pos_ < 2) {
                return status{};
            }
            const char first = buffer_[pos_];
            const char second = buffer_[pos_ + 1];
            if (first == '-' && second == '-') {
                pos_ += 2;
                complete_ = true;
                state_ = state::done;
                return status{};
            }
            if (first == '\r' && second == '\n') {
                pos_ += 2;
                status result = begin_part();
                if (!result.ok()) {
                    return result;
                }
                break;
            }
            return fail(rpc_code::protocol_error,
                        "malformed multipart delimiter line");
        }
        case state::part_header: {
            const std::size_t end = buffer_.find("\r\n\r\n", pos_);
            if (end == std::string::npos) {
                if (buffer_.size() - pos_ > limits_.max_part_header_bytes) {
                    return fail(rpc_code::resource_exhausted,
                                "multipart part header exceeds limit");
                }
                return status{};
            }
            if (end - pos_ > limits_.max_part_header_bytes) {
                return fail(rpc_code::resource_exhausted,
                            "multipart part header exceeds limit");
            }
            const std::string_view block(buffer_.data() + pos_, end - pos_);
            status result = parse_part_headers(block);
            if (!result.ok()) {
                return result;
            }
            pos_ = end + 4;
            state_ = state::part_body;
            result = handler_->on_part_begin(current_);
            if (!result.ok()) {
                return fail(result.code(), result.message());
            }
            break;
        }
        case state::part_body: {
            const std::size_t found = buffer_.find(body_delimiter_, pos_);
            if (found == std::string::npos) {
                // Retain the longest suffix that can still be a prefix of the
                // delimiter. Everything before it is part data.
                const std::size_t keep = body_delimiter_.size() - 1;
                if (buffer_.size() - pos_ > keep) {
                    const std::size_t end = buffer_.size() - keep;
                    status result = emit_data(pos_, end);
                    if (!result.ok()) {
                        return result;
                    }
                    pos_ = end;
                }
                return status{};
            }
            if (found > pos_) {
                status result = emit_data(pos_, found);
                if (!result.ok()) {
                    return result;
                }
                pos_ = found;
            }
            if (buffer_.size() - pos_ < body_delimiter_.size() + 2) {
                return status{};
            }
            const char first = buffer_[pos_ + body_delimiter_.size()];
            const char second = buffer_[pos_ + body_delimiter_.size() + 1];
            if (first == '-' && second == '-') {
                pos_ += body_delimiter_.size() + 2;
                status result = end_part();
                if (!result.ok()) {
                    return result;
                }
                complete_ = true;
                state_ = state::done;
                return status{};
            }
            if (first == '\r' && second == '\n') {
                pos_ += body_delimiter_.size() + 2;
                status result = end_part();
                if (!result.ok()) {
                    return result;
                }
                result = begin_part();
                if (!result.ok()) {
                    return result;
                }
                break;
            }
            // The byte run looked like a delimiter but is not one (for example
            // "\r\n--boundaryX"). The CR belongs to the part data; emit it and
            // keep scanning after it.
            status result = emit_data(found, found + 1);
            if (!result.ok()) {
                return result;
            }
            pos_ = found + 1;
            break;
        }
        case state::done:
            return status{};
        case state::failed:
            return status{error_};
        }
    }
}

} // namespace crpc::http
