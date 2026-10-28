// CRPC -- Oracle TNS (Transparent Network Substrate) packet framing.
//
// END-TO-END UNVERIFIED. There is no Oracle server in this project's test
// environment (no listener, no instance, no container), so nothing here has ever
// been exchanged with a database. Every field was taken from Oracle's own
// python-oracledb (thin mode) and checked against bytes that the reference
// client actually emitted; see docs/oracle-ttc-evidence.md for the citations and
// the captured vectors. Authentication is deliberately absent: it needs a
// server round trip and could not be verified at all.
//
// This is a codec, not an adapter. db::connection has no Oracle backend, and
// adding one before authentication can be validated would make the library claim
// a capability it does not have.
//
// Two framing facts are easy to get wrong and are encoded here:
//
//   * the header is 8 bytes in two layouts. Below protocol version 315 the first
//     field is a 16-bit length plus a 16-bit checksum; from 315 on it is a 32-bit
//     length and no checksum. Type and flags stay at offsets 4 and 5 in both. The
//     reference never computes a checksum -- it writes zero -- so a non-zero
//     checksum is rejected rather than silently ignored.
//   * a DATA packet carries two extra bytes of data flags after the header, so
//     its body starts at offset 10. The reference writes those flags on every
//     packet of a multi-packet request, not only the first.
#ifndef CRPC_DB_ORACLE_TNS_HPP
#define CRPC_DB_ORACLE_TNS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace oracle {

// --- Constants ---------------------------------------------------------------

inline constexpr std::size_t tns_header_size = 8;
inline constexpr std::size_t tns_data_header_size = 10;

// impl/thin/constants.pxi:34-41.
enum class tns_packet_type : std::uint8_t {
    connect = 1,
    accept = 2,
    refuse = 4,
    redirect = 5,
    data = 6,
    resend = 11,
    marker = 12,
    control = 14
};

// impl/thin/constants.pxi:31-35.
enum class tns_marker : std::uint8_t {
    break_request = 1,
    reset = 2,
    interrupt = 3
};

// impl/thin/constants.pxi:19-22.
inline constexpr std::uint8_t tns_packet_flag_redirect = 0x04;
inline constexpr std::uint8_t tns_packet_flag_tls_reneg = 0x08;

// impl/thin/constants.pxi:24-29.
inline constexpr std::uint16_t tns_data_flag_eof = 0x0040;
inline constexpr std::uint16_t tns_data_flag_end_of_request = 0x0800;
inline constexpr std::uint16_t tns_data_flag_begin_pipeline = 0x1000;
inline constexpr std::uint16_t tns_data_flag_end_of_response = 0x2000;

// impl/thin/constants.pxi:384-414.
inline constexpr std::uint16_t tns_version_desired = 319;
inline constexpr std::uint16_t tns_version_minimum = 300;
inline constexpr std::uint16_t tns_version_min_accepted = 315;
inline constexpr std::uint16_t tns_version_min_large_length = 315;
inline constexpr std::uint16_t tns_protocol_characteristics = 0x4f98;

// impl/base/connect_params.pyx:40-44, impl/thin/constants.pxi:737.
inline constexpr std::uint32_t tns_default_sdu = 8192;
inline constexpr std::size_t tns_max_connect_data = 230;
inline constexpr std::uint16_t tns_connect_service_options = 0x0401;
inline constexpr std::uint8_t tns_connect_nsi_flags = 0x84;
inline constexpr std::uint32_t tns_connect_flags_check_oob = 0x00000001;

// The reference writes 74 here although the body before the descriptor is 66
// bytes: the value is measured from the packet start, header included, and is
// never read back (impl/thin/messages/connect.pyx:145). Both are named so the
// captured vector can be reproduced exactly.
inline constexpr std::size_t tns_connect_fixed_body = 66;
inline constexpr std::uint16_t tns_connect_data_offset = 74;

// Refuse anything above this rather than buffering an attacker-chosen size.
inline constexpr std::uint32_t tns_max_packet_length = 1u << 20;
inline constexpr std::size_t tns_max_message_size = 64u * 1024u * 1024u;

// --- Header ------------------------------------------------------------------

struct tns_header {
    tns_packet_type type = tns_packet_type::data;
    std::uint8_t flags = 0;
    // Total size of the packet, header included.
    std::uint32_t length = tns_header_size;
    // Written for DATA packets only; the caller supplies it on encode and the
    // decoder leaves it zero (the assembler reads it from the body).
    std::uint16_t data_flags = 0;
};

// `large_length` selects the 32-bit length layout used from protocol version 315
// on; below that the length is 16 bits and a 16-bit checksum follows it.
[[nodiscard]] CRPC_API status encode_tns_header(std::vector<std::byte>& out,
                                                const tns_header& header,
                                                bool large_length);

// A non-zero checksum in the small layout is rejected: the reference writes zero
// and implements no checksum, so accepting the word would imply a verification
// that never happens.
[[nodiscard]] CRPC_API status decode_tns_header(span<const std::byte> in,
                                                bool large_length,
                                                tns_header& out) noexcept;

// --- Connect -----------------------------------------------------------------

struct tns_connect {
    std::uint16_t version_desired = tns_version_desired;
    std::uint16_t version_minimum = tns_version_minimum;
    std::uint16_t service_options = tns_connect_service_options;
    std::uint32_t sdu = tns_default_sdu;
    std::uint16_t characteristics = tns_protocol_characteristics;
    // 0x84 = NSI_SUPPORT_SECURITY_RENEG | NSI_NA_DISABLED, written twice
    // (impl/thin/messages/connect.pyx:123-124).
    std::uint8_t nsi_flags = tns_connect_nsi_flags;
    std::uint32_t connect_flags1 = 0;
    std::uint32_t connect_flags2 = tns_connect_flags_check_oob;
    std::uint8_t packet_flags = 0;
    // ASCII connect descriptor, e.g. `(DESCRIPTION=...)(CONNECT_DATA=...)`.
    std::string connect_data;
};

// Appends the CONNECT packet. When the descriptor is longer than
// tns_max_connect_data the reference ends the CONNECT packet early and carries
// the descriptor in a DATA packet (impl/thin/messages/connect.pyx:156-159); this
// encoder does the same and appends both packets.
// The CONNECT packet is sent before any version has been agreed, so the
// reference always writes it with the 16-bit length layout; the captured
// 273-byte vector starts with `0111 0000` (= length 273, checksum 0), not with a
// 32-bit length. The switch to the 32-bit layout happens only after ACCEPT
// (impl/thin/messages/connect.pyx:89 sets _full_packet_size), which is why the
// default here is false.
[[nodiscard]] CRPC_API status encode_connect(std::vector<std::byte>& out,
                                             const tns_connect& request,
                                             bool large_length = false);

struct tns_accept {
    std::uint16_t version = 0;
    std::uint16_t options = 0;
    std::uint8_t nsi_flags = 0;
    std::uint32_t sdu = 0;
    // Present from protocol version 318 on; zero otherwise.
    std::uint32_t flags2 = 0;
};

inline constexpr std::uint32_t tns_accept_flag_check_oob = 0x00000001;
inline constexpr std::uint32_t tns_accept_flag_fast_auth = 0x10000000;
inline constexpr std::uint32_t tns_accept_flag_end_of_response = 0x02000000;

// `body` is the packet payload without the TNS header.
[[nodiscard]] CRPC_API status decode_accept(span<const std::byte> body,
                                            tns_accept& out) noexcept;

struct tns_refuse {
    // The `(ERR=<number>)` code, or 0 when the text carries none.
    std::uint32_t code = 0;
    std::string message;
};

[[nodiscard]] CRPC_API status decode_refuse(span<const std::byte> body,
                                            tns_refuse& out);

// Redirect text is `<connect string>\0<CONNECT_DATA...>`
// (impl/thin/connection.pyx:251-270).
[[nodiscard]] CRPC_API status decode_redirect(span<const std::byte> body,
                                              std::string& text);

// --- Outbound packets --------------------------------------------------------

// Splits `body` into packets no larger than `sdu`, appending all of them. DATA
// packets carry the two-byte data flags; `packet_flags` is copied into every
// header (that is how the reference marks a retry after a redirect).
[[nodiscard]] CRPC_API status encode_tns_message(std::vector<std::byte>& out,
                                                 tns_packet_type type,
                                                 span<const std::byte> body,
                                                 std::uint32_t sdu,
                                                 bool large_length,
                                                 std::uint16_t data_flags = 0,
                                                 std::uint8_t packet_flags = 0);

// A marker packet is 8 header bytes plus the body `01 00 <marker>`.
[[nodiscard]] CRPC_API status encode_marker(std::vector<std::byte>& out,
                                            tns_marker marker, bool large_length);

// --- Inbound packets ---------------------------------------------------------

struct tns_packet {
    tns_packet_type type = tns_packet_type::data;
    std::uint8_t flags = 0;
    // Flags of the last DATA packet in the message; zero for other types.
    std::uint16_t data_flags = 0;
    // Payload without the 8-byte header and, for DATA, without the 2 data-flag
    // bytes.
    std::vector<std::byte> body;
};

enum class tns_assemble : std::uint8_t {
    // A message was produced.
    ready,
    // Not enough bytes yet; feed more and ask again.
    need_more,
    // `error()` says why.
    failed
};

// Reassembles a TNS byte stream into messages. A DATA message spans packets until
// the end-of-response flag, the EOF flag, or an END_OF_RESPONSE byte; every other
// packet type is a message of its own. A partially received message is retained
// across calls, and bytes after a complete message stay buffered for the next
// call.
class CRPC_API tns_assembler {
public:
    explicit tns_assembler(bool large_length = true) noexcept : large_(large_length) {}

    // Must be set before the first packet is parsed. A known Oracle server
    // answers with version 319+ and the large layout; the small layout exists
    // for pre-12.1 peers and is decoded but never selected automatically.
    void set_large_length(bool value) noexcept { large_ = value; }
    [[nodiscard]] bool large_length() const noexcept { return large_; }

    [[nodiscard]] status feed(span<const std::byte> data);

    [[nodiscard]] tns_assemble next_message(tns_packet& out);

    [[nodiscard]] const status& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t buffered() const noexcept;
    void clear();

private:
    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
    bool large_ = true;
    status error_;
    // Partial message: a DATA response spans packets, so the bytes collected so
    // far have to survive between calls.
    std::vector<std::byte> pending_body_;
    tns_packet_type pending_type_ = tns_packet_type::data;
    std::uint8_t pending_flags_ = 0;
    std::uint16_t pending_data_flags_ = 0;
    bool pending_ = false;
};

// True for the last packet of a response. The reference accepts three spellings
// (impl/thin/packet.pyx:59-74).
[[nodiscard]] CRPC_API bool tns_is_end_of_response(const tns_header& header,
                                                   span<const std::byte> body) noexcept;

} // namespace oracle
} // namespace db
} // namespace crpc

#endif // CRPC_DB_ORACLE_TNS_HPP