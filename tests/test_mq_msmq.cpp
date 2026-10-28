// CRPC -- MSMQ adapter unit tests.
//
// These run on every platform: the value types and the error mapping are
// platform independent by construction (the mapping constants are pinned here
// as literals). The Windows-only blocks additionally cross-check our constants
// against mq.h, so a Windows SDK bump that changed a constant fails the build
// instead of silently changing behaviour.
#include "test_framework.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/mq/msmq/queue.hpp>
#include <crpc/mq/msmq/transaction.hpp>
#include <crpc/mq/msmq/types.hpp>

#if defined(CRPC_OS_WINDOWS)
// See src/mq/msmq/queue.cpp: mq.h needs the COM `interface` keyword that
// ole2.h provides.
#include <windows.h>
#include <ole2.h>
#include <mq.h>
#endif

namespace {

using crpc::mq::msmq::acknowledgement;
using crpc::mq::msmq::max_label_length;
using crpc::mq::msmq::min_priority;
using crpc::mq::msmq::max_priority;
using crpc::mq::msmq::correlation_id_size;

#if defined(CRPC_OS_WINDOWS)

// The public constants must match what the MSMQ headers actually define; a
// changed writer or a newer SDK cannot silently diverge.
static_assert(min_priority == MQ_MIN_PRIORITY, "priority minimum");
static_assert(max_priority == MQ_MAX_PRIORITY, "priority maximum");
static_assert(correlation_id_size == PROPID_M_CORRELATIONID_SIZE, "correlation id size");
static_assert(max_label_length + 1 == MQ_MAX_MSG_LABEL_LEN, "label payload limit");
static_assert(static_cast<std::uint32_t>(crpc::mq::msmq::queue_access::receive) ==
                  static_cast<std::uint32_t>(MQ_RECEIVE_ACCESS),
              "receive access mode");
static_assert(static_cast<std::uint32_t>(crpc::mq::msmq::queue_access::send) ==
                  static_cast<std::uint32_t>(MQ_SEND_ACCESS),
              "send access mode");
static_assert(static_cast<std::uint32_t>(crpc::mq::msmq::queue_access::peek) ==
                  static_cast<std::uint32_t>(MQ_PEEK_ACCESS),
              "peek access mode");
static_assert(static_cast<std::uint32_t>(acknowledgement::positive_arrival) ==
                  static_cast<std::uint32_t>(MQMSG_ACKNOWLEDGMENT_POS_ARRIVAL),
              "positive arrival");
static_assert(static_cast<std::uint32_t>(acknowledgement::positive_receive) ==
                  static_cast<std::uint32_t>(MQMSG_ACKNOWLEDGMENT_POS_RECEIVE),
              "positive receive");
static_assert(static_cast<std::uint32_t>(acknowledgement::negative_arrival) ==
                  static_cast<std::uint32_t>(MQMSG_ACKNOWLEDGMENT_NEG_ARRIVAL),
              "negative arrival");
static_assert(static_cast<std::uint32_t>(acknowledgement::negative_receive) ==
                  static_cast<std::uint32_t>(MQMSG_ACKNOWLEDGMENT_NEG_RECEIVE),
              "negative receive");

#endif // CRPC_OS_WINDOWS

} // namespace

CRPC_TEST(msmq_priority_and_label_limits) {
    CRPC_CHECK_EQ(std::uint8_t{0}, min_priority);
    CRPC_CHECK_EQ(std::uint8_t{7}, max_priority);
    CRPC_CHECK_EQ(std::size_t{20}, correlation_id_size);
    // 249 payload code units: 250 is the MQ_MAX_MSG_LABEL_LEN buffer size that
    // includes the terminator, and a 250-character label is rejected by a real
    // service with MQ_ERROR_LABEL_TOO_LONG (covered by the integration test).
    CRPC_CHECK_EQ(std::size_t{249}, max_label_length);
}

CRPC_TEST(msmq_error_mapping_ok) {
    CRPC_CHECK_EQ(crpc::rpc_code::ok,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0)));
}

CRPC_TEST(msmq_error_mapping_queue_not_found) {
    // MQ_ERROR_QUEUE_NOT_FOUND = 0xC00E0003.
    CRPC_CHECK_EQ(crpc::rpc_code::not_found,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0003)));
}

CRPC_TEST(msmq_error_mapping_timeout_is_deadline) {
    // MQ_ERROR_IO_TIMEOUT = 0xC00E001B. A timed-out receive must not surface as
    // a fatal error; it is the dedicated timeout code.
    const auto code = crpc::mq::msmq::map_mq_error(
        static_cast<std::uint32_t>(0xC00E001B));
    CRPC_CHECK_EQ(crpc::rpc_code::deadline_exceeded, code);
    // And the timeout code is deliberately NOT retryable as an endpoint fault.
    CRPC_CHECK_FALSE(crpc::is_retryable(code));
}

CRPC_TEST(msmq_error_mapping_access_denied) {
    // MQ_ERROR_ACCESS_DENIED = 0xC00E0025.
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0025)));
}

CRPC_TEST(msmq_error_mapping_queue_exists) {
    // MQ_ERROR_QUEUE_EXISTS = 0xC00E0005.
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0005)));
}

CRPC_TEST(msmq_error_mapping_sender_cert_buffer) {
    // MQ_ERROR_SENDER_CERT_BUFFER_TOO_SMALL = 0xC00E002B. This is a caller-side
    // buffer problem, so it maps to invalid_argument, not to a retryable code.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E002B)));
}

CRPC_TEST(msmq_error_mapping_misc) {
    // invalid parameter, illegal property value, label too long, buffer overflow.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0006)));
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0018)));
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E005D)));
    // A body buffer that is too small is a size/argument mistake, not a storage
    // fault: the message stays in the queue and the caller retries with a
    // bigger buffer.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E001A)));
    // Message/lookup failures are "nothing there".
    CRPC_CHECK_EQ(crpc::rpc_code::not_found,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0088)));
}

CRPC_TEST(msmq_error_mapping_service_unavailable) {
    // MQ_ERROR_SERVICE_NOT_AVAILABLE = 0xC00E000B.
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E000B)));
}

CRPC_TEST(msmq_error_mapping_transaction_usage) {
    // MQ_ERROR_TRANSACTION_USAGE = 0xC00E0050: the caller mixed transactional
    // and non-transactional operations on one queue.
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xC00E0050)));
}

CRPC_TEST(msmq_error_mapping_unknown_falls_back_application) {
    CRPC_CHECK_EQ(crpc::rpc_code::application,
                  crpc::mq::msmq::map_mq_error(static_cast<std::uint32_t>(0xEEEEEEEE)));
}

CRPC_TEST(msmq_default_message_is_valid) {
    crpc::mq::msmq::message msg;
    CRPC_CHECK(msg.body.empty());
    CRPC_CHECK_EQ(std::uint8_t{3}, msg.priority);
    CRPC_CHECK_FALSE(msg.recoverable);
    CRPC_CHECK(msg.ack == crpc::mq::msmq::acknowledgement::none);
    CRPC_CHECK_EQ(std::uint32_t{0}, msg.app_specific);
    CRPC_CHECK_EQ(std::uint16_t{0}, msg.body_type);
    for (std::size_t i = 0; i < msg.correlation_id.size(); ++i) {
        CRPC_CHECK(msg.correlation_id[i] == std::byte{0});
    }
}

CRPC_TEST(msmq_default_received_message_is_not_present) {
    crpc::mq::msmq::received_message got;
    CRPC_CHECK_FALSE(got.present);
    CRPC_CHECK_EQ(std::uint64_t{0}, got.lookup_id);
    CRPC_CHECK(got.arrived_at == std::chrono::system_clock::time_point{});
}

CRPC_TEST_MAIN()