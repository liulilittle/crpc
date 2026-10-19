// CRPC -- Server-Sent Events framing helpers.
#include <crpc/http/sse.hpp>

#include <algorithm>
#include <cstring>

namespace crpc {
namespace http {
namespace sse {
namespace {

[[nodiscard]] bool parse_u64(std::string_view text, u64& value) noexcept {
    if (text.empty()) {
        return false;
    }
    u64 result = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (result > (~static_cast<u64>(0) - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

[[nodiscard]] bool is_continuation(unsigned char c) noexcept {
    return (c & 0xC0u) == 0x80u;
}

// Strict UTF-8 validation: no overlong forms, no surrogates, nothing above
// U+10FFFF. Control characters are rejected except LF/CR/TAB because an
// unescaped control byte (notably NUL) cannot travel safely as SSE text.
[[nodiscard]] bool valid_utf8(span<const std::byte> data) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t size = data.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char c = bytes[i];
        if (c < 0x80u) {
            if (c < 0x20u && c != '\n' && c != '\r' && c != '\t') {
                return false;
            }
            ++i;
            continue;
        }
        u32 code = 0;
        std::size_t extra = 0;
        if ((c & 0xE0u) == 0xC0u) {
            code = c & 0x1Fu;
            extra = 1;
            if (code < 0x02u) {
                return false; // overlong
            }
        } else if ((c & 0xF0u) == 0xE0u) {
            code = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8u) == 0xF0u) {
            code = c & 0x07u;
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= size) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char next = bytes[i + k];
            if (!is_continuation(next)) {
                return false;
            }
            code = (code << 6) | (next & 0x3Fu);
        }
        if ((extra == 2 && code < 0x800u) || (extra == 3 && code < 0x10000u)) {
            return false; // overlong
        }
        if (code >= 0xD800u && code <= 0xDFFFu) {
            return false; // surrogate
        }
        if (code > 0x10FFFFu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

void append_data_lines(std::string_view text, std::string& out) {
    if (text.empty()) {
        out.append("data: \n", 7);
        return;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '\n' && c != '\r') {
            continue;
        }
        out.append("data: ", 6);
        out.append(text.data() + start, i - start);
        out.push_back('\n');
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            ++i;
        }
        start = i + 1;
    }
    if (start < text.size()) {
        out.append("data: ", 6);
        out.append(text.data() + start, text.size() - start);
        out.push_back('\n');
    }
}

} // namespace

status parse_last_event_id(std::string_view text, event_id& out) noexcept {
    if (text.empty()) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "Last-Event-ID is empty")};
    }
    if (text.size() > max_event_id_chars) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "Last-Event-ID is too long")};
    }
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1 >= text.size() ||
        text.find(':', colon + 1) != std::string_view::npos) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "Last-Event-ID must be <epoch>:<seq>")};
    }
    event_id parsed;
    if (!parse_u64(text.substr(0, colon), parsed.epoch) ||
        !parse_u64(text.substr(colon + 1), parsed.seq)) {
        return status{rpc_error(rpc_code::invalid_argument,
                                "Last-Event-ID contains a non-u64 field")};
    }
    out = parsed;
    return status{};
}

status encode_event(event_id id, span<const std::byte> data, std::string& out) {
    try {
        out.append("id: ", 4);
        out += std::to_string(id.epoch);
        out.push_back(':');
        out += std::to_string(id.seq);
        out.push_back('\n');
        if (is_text(data)) {
            append_data_lines(
                std::string_view(reinterpret_cast<const char*>(data.data()),
                                 data.size()),
                out);
        } else {
            out.append("data: b64:", 10);
            out += base64_encode(data);
            out.push_back('\n');
        }
        out.push_back('\n');
    } catch (...) {
        return status{rpc_error(rpc_code::resource_exhausted,
                                "SSE event allocation failed")};
    }
    return status{};
}

status encode_event(event_id id, std::string_view text, std::string& out) {
    return encode_event(
        id,
        span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                              text.size()),
        out);
}

std::string heartbeat() {
    return std::string(":hb\n\n", 5);
}

std::string retry_hint(std::chrono::milliseconds value) {
    if (value.count() <= 0) {
        return std::string();
    }
    return std::string("retry: ") + std::to_string(value.count()) + "\n\n";
}

std::string base64_encode(span<const std::byte> data) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const u32 block = (static_cast<u32>(bytes[i]) << 16) |
                          (static_cast<u32>(bytes[i + 1]) << 8) |
                          static_cast<u32>(bytes[i + 2]);
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.push_back(alphabet[(block >> 6) & 0x3Fu]);
        out.push_back(alphabet[block & 0x3Fu]);
        i += 3;
    }
    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const u32 block = static_cast<u32>(bytes[i]) << 16;
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.append("==", 2);
    } else if (remaining == 2) {
        const u32 block = (static_cast<u32>(bytes[i]) << 16) |
                          (static_cast<u32>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.push_back(alphabet[(block >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

bool is_text(span<const std::byte> data) noexcept {
    return valid_utf8(data);
}

} // namespace sse
} // namespace http
} // namespace crpc
