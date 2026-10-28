// CRPC -- WebSocket wire codec implementation (RFC 6455 sections 5, 6 and 7).
#include <crpc/ws/frame.hpp>

#include <cstring>
#include <limits>
#include <random>

#include <crpc/endian.hpp>

namespace crpc {
namespace ws {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

void random_bytes(span<std::byte> out) {
    std::random_device device;
    for (auto& byte : out) {
        byte = static_cast<std::byte>(device() & 0xffu);
    }
}

void unmask_in_place(std::string& payload, const std::array<std::byte, 4>& mask) {
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(
            static_cast<unsigned char>(payload[i]) ^
            static_cast<unsigned char>(mask[i % 4]));
    }
}

} // namespace

bool is_control(opcode op) noexcept {
    return op == opcode::close || op == opcode::ping || op == opcode::pong;
}

bool is_data(opcode op) noexcept {
    return op == opcode::text || op == opcode::binary ||
           op == opcode::continuation;
}

bool is_valid_close_code(std::uint16_t code) noexcept {
    switch (code) {
    case 1000:
    case 1001:
    case 1002:
    case 1003:
    case 1007:
    case 1008:
    case 1009:
    case 1010:
    case 1011:
        return true;
    default:
        break;
    }
    // 1012-1014 were registered by IANA after RFC 6455; 3000-3999 is
    // registered for libraries/frameworks/applications, 4000-4999 for private
    // use. Everything else -- 1004-1006, 1015 and 1016-2999 -- is reserved and
    // must never appear on the wire.
    return (code >= 1012 && code <= 1014) || (code >= 3000 && code <= 4999);
}

bool is_valid_utf8(span<const std::byte> text) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const auto* const end = p + text.size();
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80u) {
            ++p;
            continue;
        }
        std::size_t continuation = 0;
        std::uint32_t cp = 0;
        std::uint32_t minimum = 0;
        if ((c & 0xe0u) == 0xc0u) {
            continuation = 1;
            cp = c & 0x1fu;
            minimum = 0x80u;
        } else if ((c & 0xf0u) == 0xe0u) {
            continuation = 2;
            cp = c & 0x0fu;
            minimum = 0x800u;
        } else if ((c & 0xf8u) == 0xf0u) {
            continuation = 3;
            cp = c & 0x07u;
            minimum = 0x10000u;
        } else {
            return false; // 0x80-0xBF lead byte, or an invalid prefix
        }
        if (static_cast<std::size_t>(end - p) <= continuation) {
            return false; // the sequence is truncated at the end of the input
        }
        for (std::size_t i = 1; i <= continuation; ++i) {
            const unsigned char byte = p[i];
            if ((byte & 0xc0u) != 0x80u) {
                return false; // not a continuation byte
            }
            cp = (cp << 6) | (byte & 0x3fu);
        }
        if (cp < minimum) {
            return false; // overlong encoding
        }
        if (cp >= 0xd800u && cp <= 0xdfffu) {
            return false; // UTF-16 surrogate
        }
        if (cp > 0x10ffffu) {
            return false; // beyond Unicode
        }
        p += continuation + 1;
    }
    return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
    return is_valid_utf8(as_bytes(text));
}

status encode_frame(const frame_header& header, span<const std::byte> payload,
                    std::string& out) {
    if (header.rsv != 0) {
        return status{rpc_code::encode_error,
                      "RSV bits must be zero: no extension was negotiated"};
    }
    const auto op = static_cast<unsigned>(header.op);
    if (!is_data(header.op) && !is_control(header.op)) {
        return status{rpc_code::encode_error, "an unknown WebSocket opcode"};
    }
    if (is_control(header.op)) {
        if (!header.fin) {
            return status{rpc_code::encode_error,
                          "a control frame must not be fragmented"};
        }
        if (payload.size() > 125) {
            return status{rpc_code::encode_error,
                          "a control frame payload is limited to 125 bytes"};
        }
    }
    if (payload.size() > 0x7FFFFFFFFFFFFFFFull) {
        return status{rpc_code::encode_error,
                      "a WebSocket payload exceeds the 63-bit length field"};
    }

    const std::size_t masked_header = header.masked ? 4u : 0u;
    const std::size_t extended = payload.size() <= 125 ? 0u
                                 : payload.size() <= 0xffff ? 2u : 8u;
    out.clear();
    out.reserve(2 + extended + masked_header + payload.size());

    char first = static_cast<char>(
        (header.fin ? 0x80u : 0u) | (header.rsv << 4) | op);
    out.push_back(first);

    char second = static_cast<char>(header.masked ? 0x80u : 0u);
    const std::size_t size = payload.size();
    if (size <= 125) {
        second = static_cast<char>(second | size);
        out.push_back(second);
    } else if (size <= 0xffff) {
        second = static_cast<char>(second | 0x7eu);
        out.push_back(second);
        char raw[2];
        crpc::endian::store_be16(raw, static_cast<std::uint16_t>(size));
        out.append(raw, 2);
    } else {
        second = static_cast<char>(second | 0x7fu);
        out.push_back(second);
        char raw[8];
        crpc::endian::store_be64(raw, static_cast<std::uint64_t>(size));
        out.append(raw, 8);
    }

    std::array<std::byte, 4> mask{};
    if (header.masked) {
        random_bytes(span<std::byte>(mask.data(), mask.size()));
        out.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    }

    const std::size_t payload_offset = out.size();
    out.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (header.masked) {
        char* begin = out.data() + payload_offset;
        for (std::size_t i = 0; i < payload.size(); ++i) {
            begin[i] = static_cast<char>(
                static_cast<unsigned char>(begin[i]) ^
                static_cast<unsigned char>(mask[i % 4]));
        }
    }
    return status{};
}

bool frame_decoder::fail(rpc_code code, std::string message) {
    failed_ = true;
    error_ = rpc_error(code, std::move(message));
    return false;
}

frame_decoder::frame_decoder(options opt) noexcept : opt_(opt) {}

frame_status frame_decoder::feed(span<const std::byte> data, decoded_frame& out) {
    if (failed_) {
        return frame_status::error;
    }
    if (!data.empty()) {
        buf_.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    for (;;) {
        if (buf_.size() < 2) {
            return frame_status::need_more;
        }
        const auto b0 = static_cast<unsigned char>(buf_[0]);
        const auto b1 = static_cast<unsigned char>(buf_[1]);

        const bool fin = (b0 & 0x80u) != 0;
        const std::uint8_t rsv = static_cast<std::uint8_t>((b0 >> 4) & 0x07u);
        const std::uint8_t opcode_raw = static_cast<std::uint8_t>(b0 & 0x0fu);
        const bool masked = (b1 & 0x80u) != 0;
        std::uint64_t length = b1 & 0x7fu;

        std::size_t header = 2;
        if (length == 126) {
            header += 2;
        } else if (length == 127) {
            header += 8;
        }
        if (masked) {
            header += 4;
        }
        if (buf_.size() < header) {
            return frame_status::need_more;
        }

        if (length == 126) {
            length = crpc::endian::load_be16(buf_.data() + 2);
            if (length <= 125) {
                (void)fail(rpc_code::protocol_error,
                           "a length below 126 must use the 7-bit encoding");
                return frame_status::error;
            }
        } else if (length == 127) {
            const std::uint64_t wide = crpc::endian::load_be64(buf_.data() + 2);
            if ((wide & (1ull << 63)) != 0) {
                (void)fail(rpc_code::protocol_error,
                           "the top bit of a 64-bit length must be zero");
                return frame_status::error;
            }
            if (wide <= 0xffff) {
                (void)fail(rpc_code::protocol_error,
                           "a length at or below 65535 must use the 16-bit encoding");
                return frame_status::error;
            }
            length = wide;
        }
        if (length > 0x7FFFFFFFFFFFFFFFull) {
            (void)fail(rpc_code::protocol_error,
                       "a WebSocket length exceeds the 63-bit field");
            return frame_status::error;
        }

        opcode op;
        switch (opcode_raw) {
        case 0x0:
            op = opcode::continuation;
            break;
        case 0x1:
            op = opcode::text;
            break;
        case 0x2:
            op = opcode::binary;
            break;
        case 0x8:
            op = opcode::close;
            break;
        case 0x9:
            op = opcode::ping;
            break;
        case 0xA:
            op = opcode::pong;
            break;
        default:
            (void)fail(rpc_code::protocol_error,
                       "a reserved WebSocket opcode");
            return frame_status::error;
        }
        if (rsv != 0) {
            (void)fail(rpc_code::protocol_error,
                       "an RSV bit is set with no extension negotiated");
            return frame_status::error;
        }
        if (is_control(op)) {
            if (!fin) {
                (void)fail(rpc_code::protocol_error,
                           "a control frame must not be fragmented");
                return frame_status::error;
            }
            if (length > 125) {
                (void)fail(rpc_code::protocol_error,
                           "a control frame payload exceeds 125 bytes");
                return frame_status::error;
            }
        }
        if (masked != opt_.expect_masked) {
            (void)fail(rpc_code::protocol_error,
                       masked ? "an unexpected client mask on a frame"
                              : "a missing client mask on a frame");
            return frame_status::error;
        }
        if (opt_.max_frame_payload != 0 && length > opt_.max_frame_payload) {
            (void)fail(rpc_code::resource_exhausted,
                       "a WebSocket frame exceeds the configured ceiling");
            return frame_status::error;
        }

        if (length > (std::numeric_limits<std::size_t>::max)() - header) {
            (void)fail(rpc_code::protocol_error,
                       "a WebSocket frame is too large to buffer");
            return frame_status::error;
        }
        const std::size_t total =
            header + static_cast<std::size_t>(length);
        if (buf_.size() < total) {
            return frame_status::need_more;
        }

        std::array<std::byte, 4> mask{};
        if (masked) {
            std::memcpy(mask.data(), buf_.data() + header - 4, 4);
        }
        std::string payload;
        payload.append(buf_, header, static_cast<std::size_t>(length));
        if (masked) {
            unmask_in_place(payload, mask);
        }
        buf_.erase(0, total);

        out.fin = fin;
        out.rsv = rsv;
        out.op = op;
        out.payload = std::move(payload);
        return frame_status::frame;
    }
}

const rpc_error& frame_decoder::error() const noexcept { return error_; }

std::size_t frame_decoder::buffered() const noexcept { return buf_.size(); }

void frame_decoder::reset() noexcept {
    buf_.clear();
    error_ = rpc_error{};
    failed_ = false;
}

} // namespace ws
} // namespace crpc