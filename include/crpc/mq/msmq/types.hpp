// CRPC -- MSMQ message value types.
//
// This header is platform independent: it never includes a Windows header, so a
// consumer on any platform can hold and compare `message`/`received_message`
// values. Everything that touches the MSMQ API lives in the implementation
// file, behind a `CRPC_OS_WINDOWS` guard.
//
// Priority and label follow the MSMQ constants (MQ_MIN_PRIORITY..MQ_MAX_PRIORITY
// and MQ_MAX_MSG_LABEL_LEN). The label limit is a *WCHAR* count in MSMQ terms
// and UTF-16 code units on the wire; a UTF-8 label that is longer is rejected
// with `encode_error` rather than silently truncated.
#ifndef CRPC_MQ_MSMQ_TYPES_HPP
#define CRPC_MQ_MSMQ_TYPES_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace mq {
namespace msmq {

// Message priority range, matching MSMQ's MQ_MIN_PRIORITY / MQ_MAX_PRIORITY.
inline constexpr std::uint8_t min_priority = 0;
inline constexpr std::uint8_t max_priority = 7;

// PROPID_M_CORRELATIONID_SIZE (20 bytes). A message with no correlation
// identifier carries all-zero bytes.
inline constexpr std::size_t correlation_id_size = 20;

// The label payload limit this client enforces, measured on a real service:
// MQ_MAX_MSG_LABEL_LEN (250) is the buffer size that leaves one WCHAR for the
// terminator, and the service rejects a 250-character label with
// MQ_ERROR_LABEL_TOO_LONG (verified against the local MSMQ). A disjoint payload
// therefore fits in 249 UTF-16 code units.
inline constexpr std::size_t max_label_length = 249;

// PROPID_M_ACKNOWLEDGE values (MQMSG_ACKNOWLEDGMENT_*). Acknowledgement
// messages are delivered to the administration queue named by
// `message::admin_queue`; without one the acknowledgment cannot be delivered.
enum class acknowledgement : std::uint32_t {
    none = 0x00,
    positive_arrival = 0x01,
    positive_receive = 0x02,
    negative_arrival = 0x04,
    negative_receive = 0x08,
};

// One outbound message. The body is always sent as an opaque binary buffer
// (PROPID_M_BODY with PROPID_M_BODY_TYPE VT_BINARY), so any byte sequence
// round-trips unchanged.
struct message {
    std::vector<std::byte> body;
    std::string label;
    std::uint8_t priority = 3;
    // false = express delivery, true = recoverable (PROPID_M_DELIVERY
    // MQMSG_DELIVERY_RECOVERABLE).
    bool recoverable = false;
    acknowledgement ack = acknowledgement::none;
    // Format name of the administration queue acknowledgements are sent to.
    // Empty when `ack` is `none`.
    std::string admin_queue;
    // PROPID_M_APPSPECIFIC, a caller-defined value carried verbatim.
    std::uint32_t app_specific = 0;
    // PROPID_M_BODY_TYPE as read back on receive. Senders keep this zero
    // (binary); it is filled in by `received_message`.
    std::uint16_t body_type = 0;
    std::array<std::byte, correlation_id_size> correlation_id{};
};

// One received message. `present` distinguishes "the wait timed out" (a normal
// outcome for an empty queue) from a real delivery, mirroring the AMQP
// adapter's `delivery::present`.
struct received_message {
    bool present = false;
    message msg;
    // PROPID_M_LOOKUPID, unique per message on this queue.
    std::uint64_t lookup_id = 0;
    // PROPID_M_ARRIVEDTIME (UTC timestamp at which the message arrived).
    std::chrono::system_clock::time_point arrived_at{};
};

// Maps an MSMQ HRESULT (an MQ_ERROR_* / MQ_INFORMATION_* value, carried as an
// unsigned 32-bit integer so the header stays platform independent) to the
// library's error model. Unknown values map to `application`.
//
// The important classification is MQ_ERROR_IO_TIMEOUT: a timed-out receive is
// a normal "nothing arrived" condition, so it maps to the dedicated timeout
// code (`deadline_exceeded`) rather than to a fatal error. The receive
// wrappers translate that code back into `present = false`.
[[nodiscard]] CRPC_API rpc_code map_mq_error(std::uint32_t hr) noexcept;

} // namespace msmq
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_MSMQ_TYPES_HPP