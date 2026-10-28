// CRPC -- Oracle TNS packet framing. See tns.hpp for provenance and scope.
#include <crpc/db/oracle/tns.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace crpc {
namespace db {
namespace oracle {
namespace {

constexpr std::byte byte_of(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

std::uint8_t u8_of(std::byte value) noexcept {
    return static_cast<std::uint8_t>(value);
}

void put_u8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(byte_of(value));
}

void put_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>(value >> 8)));
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
}

void put_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 24) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 16) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
}

void put_be64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(byte_of(static_cast<std::uint8_t>((value >> shift) & 0xffu)));
    }
}

std::uint16_t get_be16(const std::byte* in) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(u8_of(in[0])) << 8) |
        static_cast<std::uint16_t>(u8_of(in[1])));
}

std::uint32_t get_be32(const std::byte* in) noexcept {
    return (static_cast<std::uint32_t>(u8_of(in[0])) << 24) |
           (static_cast<std::uint32_t>(u8_of(in[1])) << 16) |
           (static_cast<std::uint32_t>(u8_of(in[2])) << 8) |
           static_cast<std::uint32_t>(u8_of(in[3]));
}

bool known_packet_type(std::uint8_t value) noexcept {
    switch (value) {
    case 1:
    case 2:
    case 4:
    case 5:
    case 6:
    case 11:
    case 12:
    case 14:
        return true;
    default:
        return false;
    }
}

// Writes one packet. `length` must already count the header, the two data-flag
// bytes for a DATA packet, and the body.
void append_packet(std::vector<std::byte>& out, tns_packet_type type,
                   span<const std::byte> body, bool large_length,
                   std::uint8_t packet_flags, std::uint32_t length,
                   std::uint16_t data_flags) {
    if (large_length) {
        put_be32(out, length);
    } else {
        put_be16(out, static_cast<std::uint16_t>(length));
        // Checksum: the reference writes zero and verifies nothing, so this
        // codec does not claim to compute one.
        put_be16(out, 0);
    }
    put_u8(out, static_cast<std::uint8_t>(type));
    put_u8(out, packet_flags);
    put_be16(out, 0); // reserved / header checksum
    if (type == tns_packet_type::data) {
        put_be16(out, data_flags);
    }
    out.reserve(out.size() + body.size());
    for (std::byte b : body) {
        out.push_back(b);
    }
}

void append_text(std::vector<std::byte>& out, const std::string& text) {
    out.reserve(out.size() + text.size());
    for (char c : text) {
        out.push_back(
            byte_of(static_cast<std::uint8_t>(static_cast<unsigned char>(c))));
    }
}

void append_span(std::vector<std::byte>& out, span<const std::byte> body) {
    out.reserve(out.size() + body.size());
    for (std::byte b : body) {
        out.push_back(b);
    }
}

} // namespace

// --- Header ------------------------------------------------------------------

status encode_tns_header(std::vector<std::byte>& out, const tns_header& header,
                         bool large_length) {
    if (header.length < tns_header_size || header.length > tns_max_packet_length) {
        return status{rpc_code::encode_error,
                      "TNS packet length is outside 8..1048576"};
    }
    if (!large_length && header.length > 0xffffu) {
        return status{rpc_code::encode_error,
                      "TNS small-layout length does not fit 16 bits"};
    }
    if (header.type == tns_packet_type::data && header.length < tns_data_header_size) {
        return status{rpc_code::encode_error,
                      "TNS DATA packet length leaves no room for its data flags"};
    }
    if (large_length) {
        put_be32(out, header.length);
    } else {
        put_be16(out, static_cast<std::uint16_t>(header.length));
        put_be16(out, 0);
    }
    put_u8(out, static_cast<std::uint8_t>(header.type));
    put_u8(out, header.flags);
    put_be16(out, 0);
    if (header.type == tns_packet_type::data) {
        put_be16(out, header.data_flags);
    }
    return status{};
}

status decode_tns_header(span<const std::byte> in, bool large_length,
                         tns_header& out) noexcept {
    if (in.size() < tns_header_size) {
        return status{rpc_code::invalid_argument, "TNS header needs 8 bytes"};
    }
    std::uint32_t length = 0;
    if (large_length) {
        length = get_be32(in.data());
    } else {
        length = get_be16(in.data());
        if (get_be16(in.data() + 2) != 0) {
            return status{
                rpc_code::protocol_error,
                "TNS checksum is set; checksummed traffic is not implemented"};
        }
    }
    if (length < tns_header_size) {
        return status{rpc_code::protocol_error,
                      "TNS packet length is below the header size"};
    }
    if (length > tns_max_packet_length) {
        return status{rpc_code::protocol_error, "TNS packet length is too large"};
    }
    const std::uint8_t type = u8_of(in[4]);
    if (!known_packet_type(type)) {
        return status{rpc_code::protocol_error, "unknown TNS packet type"};
    }
    out.type = static_cast<tns_packet_type>(type);
    out.flags = u8_of(in[5]);
    out.length = length;
    out.data_flags = 0;
    return status{};
}

bool tns_is_end_of_response(const tns_header& header,
                            span<const std::byte> body) noexcept {
    if (header.type != tns_packet_type::data) {
        return true;
    }
    if ((header.data_flags & tns_data_flag_end_of_response) != 0 ||
        (header.data_flags & tns_data_flag_eof) != 0) {
        return true;
    }
    // impl/thin/packet.pyx:71-74: a header-only response whose body is exactly
    // END_OF_RESPONSE also terminates it.
    return body.size() == 3 && u8_of(body[0]) == 29;
}

// --- Connect -----------------------------------------------------------------

status encode_connect(std::vector<std::byte>& out, const tns_connect& request,
                      bool large_length) {
    if (request.sdu < tns_data_header_size || request.sdu > 0xffffu) {
        return status{rpc_code::invalid_argument, "TNS SDU is outside 10..65535"};
    }
    if (request.connect_data.size() > 0xffffu) {
        return status{rpc_code::invalid_argument,
                      "connect data does not fit the 16-bit length field"};
    }
    const auto descriptor_len =
        static_cast<std::uint16_t>(request.connect_data.size());
    const auto sdu16 = static_cast<std::uint16_t>(request.sdu);

    std::vector<std::byte> body;
    body.reserve(tns_connect_fixed_body + request.connect_data.size());
    put_be16(body, request.version_desired);
    put_be16(body, request.version_minimum);
    put_be16(body, request.service_options);
    put_be16(body, sdu16); // SDU
    put_be16(body, sdu16); // TDU; the reference writes SDU twice
    put_be16(body, request.characteristics);
    put_be16(body, 0); // line turnaround
    put_be16(body, 1); // constant
    put_be16(body, descriptor_len);
    put_be16(body, tns_connect_data_offset);
    put_be32(body, 0); // max receivable data
    put_u8(body, request.nsi_flags);
    put_u8(body, request.nsi_flags);
    put_be64(body, 0);
    put_be64(body, 0);
    put_be64(body, 0);
    put_be32(body, request.sdu); // SDU, 32-bit
    put_be32(body, request.sdu); // TDU, 32-bit
    put_be32(body, request.connect_flags1);
    put_be32(body, request.connect_flags2);
    if (body.size() != tns_connect_fixed_body) {
        return status{rpc_code::internal, "connect body prefix is not 66 bytes"};
    }

    if (request.connect_data.size() > tns_max_connect_data) {
        // End the CONNECT packet at the fixed part and continue in a DATA
        // packet, which is what impl/thin/messages/connect.pyx:156-159 does.
        const auto connect_length =
            static_cast<std::uint32_t>(tns_header_size + body.size());
        append_packet(out, tns_packet_type::connect,
                      span<const std::byte>(body.data(), body.size()), large_length,
                      request.packet_flags, connect_length, 0);
        std::vector<std::byte> text;
        text.reserve(request.connect_data.size());
        append_text(text, request.connect_data);
        const auto data_length =
            static_cast<std::uint32_t>(tns_data_header_size + text.size());
        append_packet(out, tns_packet_type::data,
                      span<const std::byte>(text.data(), text.size()), large_length,
                      request.packet_flags, data_length, 0);
        return status{};
    }

    const std::size_t text_at = body.size();
    body.resize(text_at + request.connect_data.size());
    if (!request.connect_data.empty()) {
        std::memcpy(body.data() + text_at, request.connect_data.data(),
                    request.connect_data.size());
    }
    const auto length = static_cast<std::uint32_t>(tns_header_size + body.size());
    append_packet(out, tns_packet_type::connect,
                  span<const std::byte>(body.data(), body.size()), large_length,
                  request.packet_flags, length, 0);
    return status{};
}

// --- Inbound control packets -------------------------------------------------

status decode_accept(span<const std::byte> body, tns_accept& out) noexcept {
    if (body.size() < 28) {
        return status{rpc_code::protocol_error, "ACCEPT body is too short"};
    }
    const std::byte* in = body.data();
    out.version = get_be16(in);
    out.options = get_be16(in + 2);
    out.nsi_flags = u8_of(in[14]);
    out.sdu = get_be32(in + 24);
    out.flags2 = 0;
    if (out.version >= 318) {
        if (body.size() < 37) {
            return status{rpc_code::protocol_error,
                          "ACCEPT body lacks the flags2 word"};
        }
        out.flags2 = get_be32(in + 33);
    }
    if (out.version < tns_version_min_accepted) {
        return status{rpc_code::protocol_error,
                      "server protocol version is below 315"};
    }
    if (out.sdu < tns_data_header_size) {
        return status{rpc_code::protocol_error, "server SDU is too small"};
    }
    return status{};
}

status decode_refuse(span<const std::byte> body, tns_refuse& out) {
    if (body.size() < 4) {
        return status{rpc_code::protocol_error, "REFUSE body is too short"};
    }
    const std::uint16_t length = get_be16(body.data() + 2);
    if (static_cast<std::size_t>(length) > body.size() - 4) {
        return status{rpc_code::protocol_error, "REFUSE text runs past the packet"};
    }
    out.message.assign(reinterpret_cast<const char*>(body.data() + 4), length);
    out.code = 0;
    const std::string marker = "(ERR=";
    const std::size_t open = out.message.find(marker);
    if (open == std::string::npos) {
        return status{};
    }
    const std::size_t close = out.message.find(')', open + marker.size());
    if (close == std::string::npos || close == open + marker.size()) {
        return status{};
    }
    std::uint32_t value = 0;
    for (std::size_t i = open + marker.size(); i < close; ++i) {
        const char c = out.message[i];
        if (c < '0' || c > '9') {
            return status{};
        }
        value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    out.code = value;
    return status{};
}

status decode_redirect(span<const std::byte> body, std::string& text) {
    if (body.size() < 2) {
        return status{rpc_code::protocol_error, "REDIRECT body is too short"};
    }
    const std::uint16_t length = get_be16(body.data());
    if (static_cast<std::size_t>(length) > body.size() - 2) {
        return status{rpc_code::protocol_error, "REDIRECT data runs past the packet"};
    }
    text.assign(reinterpret_cast<const char*>(body.data() + 2), length);
    return status{};
}

// --- Outbound messages -------------------------------------------------------

status encode_tns_message(std::vector<std::byte>& out, tns_packet_type type,
                          span<const std::byte> body, std::uint32_t sdu,
                          bool large_length, std::uint16_t data_flags,
                          std::uint8_t packet_flags) {
    if (sdu > 0xffffu) {
        return status{rpc_code::invalid_argument, "TNS SDU does not fit 16 bits"};
    }
    if (body.empty()) {
        return status{};
    }
    const std::size_t overhead =
        tns_header_size + (type == tns_packet_type::data ? 2u : 0u);
    if (sdu < overhead) {
        return status{rpc_code::invalid_argument, "TNS SDU leaves no body room"};
    }
    // END_OF_RESPONSE ends the message, so it belongs on the final packet only;
    // the other flags describe the request and repeat on every packet, which is
    // what the reference's write buffer does.
    const auto steady = static_cast<std::uint16_t>(
        data_flags & static_cast<std::uint16_t>(~tns_data_flag_end_of_response));
    const std::size_t capacity = sdu - overhead;
    std::size_t offset = 0;
    while (offset < body.size()) {
        const std::size_t count = std::min(capacity, body.size() - offset);
        const bool last = offset + count == body.size();
        const std::uint16_t flags = last ? data_flags : steady;
        const auto length = static_cast<std::uint32_t>(overhead + count);
        append_packet(out, type, span<const std::byte>(body.data() + offset, count),
                      large_length, packet_flags, length, flags);
        offset += count;
    }
    return status{};
}

status encode_marker(std::vector<std::byte>& out, tns_marker marker,
                     bool large_length) {
    if (large_length) {
        put_be32(out, tns_header_size + 3);
    } else {
        put_be16(out, static_cast<std::uint16_t>(tns_header_size + 3));
        put_be16(out, 0);
    }
    put_u8(out, static_cast<std::uint8_t>(tns_packet_type::marker));
    put_u8(out, 0);
    put_be16(out, 0);
    put_u8(out, 1);
    put_u8(out, 0);
    put_u8(out, static_cast<std::uint8_t>(marker));
    return status{};
}

// --- Assembler ---------------------------------------------------------------

status tns_assembler::feed(span<const std::byte> data) {
    if (!error_.ok()) {
        return error_;
    }
    if (data.empty()) {
        return status{};
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
    append_span(buffer_, data);
    return status{};
}

std::size_t tns_assembler::buffered() const noexcept {
    return buffer_.size() - offset_;
}

void tns_assembler::clear() {
    buffer_.clear();
    offset_ = 0;
    pending_body_.clear();
    pending_ = false;
    pending_data_flags_ = 0;
    error_ = status{};
}

tns_assemble tns_assembler::next_message(tns_packet& out) {
    if (!error_.ok()) {
        return tns_assemble::failed;
    }
    for (;;) {
        const std::size_t available = buffer_.size() - offset_;
        if (available < tns_header_size) {
            return tns_assemble::need_more;
        }
        const std::byte* base = buffer_.data() + offset_;
        tns_header header;
        const status decoded =
            decode_tns_header(span<const std::byte>(base, available), large_, header);
        if (!decoded.ok()) {
            error_ = decoded;
            pending_ = false;
            pending_body_.clear();
            return tns_assemble::failed;
        }
        if (available < header.length) {
            return tns_assemble::need_more;
        }

        std::size_t body_offset = tns_header_size;
        std::uint16_t data_flags = 0;
        if (header.type == tns_packet_type::data) {
            if (header.length < tns_data_header_size) {
                error_ = status{rpc_code::protocol_error,
                                "TNS DATA packet lacks its data flags"};
                pending_ = false;
                pending_body_.clear();
                return tns_assemble::failed;
            }
            data_flags = get_be16(base + tns_header_size);
            body_offset = tns_data_header_size;
        }

        // A packet of another type cannot be attached to the message in
        // progress, so hand that one out and leave this packet for the next
        // call.
        if (pending_ && header.type != pending_type_) {
            out.type = pending_type_;
            out.flags = pending_flags_;
            out.data_flags = pending_data_flags_;
            out.body = std::move(pending_body_);
            pending_body_ = {};
            pending_ = false;
            return tns_assemble::ready;
        }

        const std::size_t body_size = header.length - body_offset;
        if (pending_body_.size() + body_size > tns_max_message_size) {
            error_ = status{rpc_code::protocol_error,
                            "TNS message exceeds the reassembly bound"};
            pending_ = false;
            pending_body_.clear();
            return tns_assemble::failed;
        }

        if (!pending_) {
            pending_ = true;
            pending_type_ = header.type;
            pending_flags_ = header.flags;
            pending_data_flags_ = 0;
        }
        append_span(pending_body_,
                    span<const std::byte>(base + body_offset, body_size));
        offset_ += header.length;

        bool complete = header.type != tns_packet_type::data;
        if (header.type == tns_packet_type::data) {
            pending_data_flags_ = data_flags;
            if ((data_flags & tns_data_flag_end_of_response) != 0 ||
                (data_flags & tns_data_flag_eof) != 0 ||
                (body_size == 3 && u8_of(base[body_offset]) == 29)) {
                complete = true;
            }
        }
        if (!complete) {
            // A DATA message without an end flag continues in the next packet.
            // Servers older than 23 do not set the flag at all; finding message
            // boundaries there needs the TTC parser, which this codec does not
            // implement, so the caller simply waits for more bytes.
            continue;
        }
        out.type = pending_type_;
        out.flags = pending_flags_;
        out.data_flags =
            pending_type_ == tns_packet_type::data ? pending_data_flags_ : 0;
        out.body = std::move(pending_body_);
        pending_body_ = {};
        pending_ = false;
        return tns_assemble::ready;
    }
}

} // namespace oracle
} // namespace db
} // namespace crpc