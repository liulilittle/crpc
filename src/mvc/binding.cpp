// CRPC -- MVC binding helpers (string parsing and multipart).
#include <crpc/mvc/binding.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crpc::mvc {

namespace {

bool ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z';
}

char to_lower(char c) noexcept {
    return ascii_lower(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view text) noexcept {
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

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (to_lower(text[i]) != to_lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

// Percent-decodes one form component. '+' becomes a space before decoding, as
// application/x-www-form-urlencoded and query strings require.
status decode_component(std::string_view raw, std::string& out) {
    std::string expanded;
    expanded.reserve(raw.size());
    for (const char c : raw) {
        expanded.push_back(c == '+' ? ' ' : c);
    }
    return http::percent_decode(expanded, out);
}

status parse_pairs(std::string_view text, route_params& out, std::size_t limit) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t amp = text.find('&', start);
        const std::string_view pair =
            amp == std::string_view::npos ? text.substr(start)
                                          : text.substr(start, amp - start);
        if (!pair.empty()) {
            if (out.entries.size() >= limit) {
                return status{rpc_error(rpc_code::resource_exhausted,
                                        "too many name/value pairs")};
            }
            const std::size_t eq = pair.find('=');
            const std::string_view raw_key =
                eq == std::string_view::npos ? pair : pair.substr(0, eq);
            const std::string_view raw_value =
                eq == std::string_view::npos ? std::string_view{}
                                             : pair.substr(eq + 1);
            std::string key;
            std::string value;
            const status key_status = decode_component(raw_key, key);
            if (!key_status.ok()) {
                return key_status;
            }
            const status value_status = decode_component(raw_value, value);
            if (!value_status.ok()) {
                return value_status;
            }
            out.entries.emplace_back(std::move(key), std::move(value));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }
    return status{};
}

// Reads a quoted or bare parameter value: name="value" or name=value.
std::string_view parameter_value(std::string_view header,
                                 std::string_view name) noexcept {
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t next = header.find(';', pos);
        const std::string_view piece =
            next == std::string_view::npos ? header.substr(pos)
                                           : header.substr(pos, next - pos);
        const std::string_view trimmed = trim(piece);
        const std::size_t eq = trimmed.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = trim(trimmed.substr(0, eq));
            if (key.size() == name.size() &&
                starts_with_ci(key, name) && key.size() == name.size()) {
                std::string_view value = trim(trimmed.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"' &&
                    value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    return std::string_view{};
}

// Extracts the value of a Content-Disposition parameter (name or filename).
std::string_view disposition_parameter(std::string_view value,
                                       std::string_view key) noexcept {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t next = value.find(';', pos);
        const std::string_view piece =
            next == std::string_view::npos ? value.substr(pos)
                                           : value.substr(pos, next - pos);
        const std::string_view trimmed = trim(piece);
        const std::size_t eq = trimmed.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view name = trim(trimmed.substr(0, eq));
            if (name.size() == key.size() && starts_with_ci(name, key) &&
                name.size() == key.size()) {
                std::string_view out = trim(trimmed.substr(eq + 1));
                if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
                    out = out.substr(1, out.size() - 2);
                }
                return out;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    return std::string_view{};
}

} // namespace

const std::string* route_params::find(std::string_view name) const noexcept {
    for (const auto& entry : entries) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

const binding_limits& default_binding_limits() noexcept {
    static const binding_limits limits{};
    return limits;
}

std::string_view media_type(std::string_view content_type) noexcept {
    const std::size_t semicolon = content_type.find(';');
    const std::string_view head =
        semicolon == std::string_view::npos
            ? content_type
            : content_type.substr(0, semicolon);
    // The returned view borrows from the caller's buffer; lower-casing requires
    // storage, so callers compare through a case-insensitive helper instead.
    return trim(head);
}

status parse_query(std::string_view query, route_params& out,
                   std::size_t limit) {
    out.entries.clear();
    return parse_pairs(query, out, limit);
}

status parse_form(std::string_view body, route_params& out, std::size_t limit) {
    out.entries.clear();
    return parse_pairs(body, out, limit);
}

const file_part* find_file(const std::vector<file_part>& files,
                           std::string_view name) noexcept {
    for (const file_part& part : files) {
        if (part.name == name) {
            return &part;
        }
    }
    return nullptr;
}

status parse_multipart(std::string_view body, std::string_view content_type,
                       const binding_limits& limits,
                       std::vector<file_part>& files, route_params& fields) {
    files.clear();
    fields.entries.clear();

    const std::string_view boundary = parameter_value(content_type, "boundary");
    if (boundary.empty()) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "multipart body has no boundary")};
    }

    std::string delimiter;
    delimiter.reserve(boundary.size() + 2);
    delimiter.append("--");
    delimiter.append(boundary.data(), boundary.size());

    std::size_t pos = body.find(delimiter);
    if (pos == std::string_view::npos) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "multipart boundary not found in body")};
    }
    pos += delimiter.size();

    std::size_t total_bytes = 0;
    std::size_t part_count = 0;

    while (pos < body.size()) {
        if (body.size() - pos >= 2 && body[pos] == '-' && body[pos + 1] == '-') {
            break; // closing delimiter
        }
        if (body.size() - pos >= 2 && body[pos] == '\r' && body[pos + 1] == '\n') {
            pos += 2;
        }

        const std::size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string_view::npos) {
            return status{rpc_error(rpc_code::invalid_argument,
                                    "multipart part header is unterminated")};
        }
        const std::string_view header_block =
            body.substr(pos, header_end - pos);
        if (header_block.size() > limits.max_part_header_bytes) {
            return status{rpc_error(rpc_code::resource_exhausted,
                                    "multipart part header exceeds limit")};
        }

        const std::size_t content_start = header_end + 4;
        const std::string next_delimiter = "\r\n" + delimiter;
        const std::size_t content_end = body.find(next_delimiter, content_start);
        if (content_end == std::string_view::npos) {
            return status{rpc_error(rpc_code::invalid_argument,
                                    "multipart part is unterminated")};
        }
        const std::string_view content =
            body.substr(content_start, content_end - content_start);

        if (part_count >= limits.max_parts) {
            return status{rpc_error(rpc_code::resource_exhausted,
                                    "too many multipart parts")};
        }
        if (content.size() > limits.max_part_bytes) {
            return status{rpc_error(rpc_code::resource_exhausted,
                                    "multipart part exceeds limit")};
        }
        total_bytes += content.size();
        if (total_bytes > limits.max_total_upload_bytes) {
            return status{rpc_error(rpc_code::resource_exhausted,
                                    "multipart upload exceeds limit")};
        }

        std::string_view part_name;
        std::string_view filename;
        std::string_view part_type;
        std::size_t line_start = 0;
        while (line_start < header_block.size()) {
            std::size_t line_end = header_block.find("\r\n", line_start);
            if (line_end == std::string_view::npos) {
                line_end = header_block.size();
            }
            const std::string_view line =
                header_block.substr(line_start, line_end - line_start);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string_view field_name = trim(line.substr(0, colon));
                const std::string_view field_value = trim(line.substr(colon + 1));
                if (field_name.size() == 19 &&
                    starts_with_ci(field_name, "content-disposition")) {
                    part_name = disposition_parameter(field_value, "name");
                    filename = disposition_parameter(field_value, "filename");
                } else if (field_name.size() == 12 &&
                           starts_with_ci(field_name, "content-type")) {
                    part_type = field_value;
                }
            }
            line_start = line_end + 2;
        }

        if (!filename.empty()) {
            file_part part;
            part.name.assign(part_name.data(), part_name.size());
            part.filename.assign(filename.data(), filename.size());
            part.content_type.assign(part_type.data(), part_type.size());
            part.data.assign(content.data(), content.size());
            files.push_back(std::move(part));
        } else if (!part_name.empty()) {
            if (fields.entries.size() >= limits.max_form_fields) {
                return status{rpc_error(rpc_code::resource_exhausted,
                                        "too many multipart form fields")};
            }
            fields.entries.emplace_back(std::string(part_name),
                                        std::string(content));
        }

        ++part_count;
        pos = content_end + next_delimiter.size();
    }

    return status{};
}

} // namespace crpc::mvc
