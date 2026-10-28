// CRPC -- synchronous MSMQ queue client.
//
// One `queue` owns one MSMQ queue handle (MQOpenQueue). Path resolution uses
// MQPathNameToFormatName rather than string surgery: a path such as
// `.\private$\name` or a public queue path is resolved by the MSMQ runtime,
// which is what makes remote/DNS-named destinations work. A path that already
// carries a format-name prefix (`FORMATNAME:` / `DIRECT=`) is used verbatim.
//
// All methods are synchronous and value-returning; nothing here throws. A
// `queue` is not safe to use concurrently.
#ifndef CRPC_MQ_MSMQ_QUEUE_HPP
#define CRPC_MQ_MSMQ_QUEUE_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/mq/msmq/types.hpp>

namespace crpc {
namespace mq {
namespace msmq {

class transaction;

// MQOpenQueue access modes (MQ_RECEIVE_ACCESS / MQ_SEND_ACCESS /
// MQ_PEEK_ACCESS). Chosen values match the MSMQ constants. Pass exactly one:
// this host's Message Queuing service rejects a combined value with
// MQ_ERROR_UNSUPPORTED_ACCESS_MODE, and a receive-access handle cannot send
// while a send-access handle cannot receive. Open a second queue when both
// directions are needed.
namespace queue_access {
inline constexpr std::uint32_t receive = 0x00000001;
inline constexpr std::uint32_t send = 0x00000002;
inline constexpr std::uint32_t peek = 0x00000020;
} // namespace queue_access

struct queue_options {
    // A queue path (`.\private$\name`, a public queue path) or an already
    // resolved format name (`FORMATNAME:...`, `DIRECT=OS:...`).
    std::string path;
    // MQOpenQueue access value, exactly one of `queue_access`.
    std::uint32_t access = queue_access::receive;
};

// One open MSMQ queue. The underlying handle is closed by the destructor (and
// by `close`), so a queue the caller does not close does not leak.
class CRPC_API queue {
public:
    // Opens (never creates) the queue named by `options.path` and resolves the
    // path through MQPathNameToFormatName when it is not already a format name.
    [[nodiscard]] static result<std::unique_ptr<queue>> open(
        const queue_options& options);

    ~queue();

    queue(const queue&) = delete;
    queue& operator=(const queue&) = delete;
    queue(queue&&) = delete;
    queue& operator=(queue&&) = delete;

    // Sends one message. A priority outside 0..7 or a label that does not fit
    // the MSMQ limit is `invalid_argument` / `encode_error`. Sending to a
    // transactional queue without a transaction fails with the mapped
    // MQ_ERROR_TRANSACTION_USAGE.
    [[nodiscard]] status send(const message& msg);
    [[nodiscard]] status send(const message& msg, transaction& tx);

    // Receives one message, waiting up to `timeout`. A timeout is a normal
    // value (`present == false`), not an error.
    [[nodiscard]] result<received_message> receive(
        std::chrono::milliseconds timeout);
    // Transactional receive: the message leaves the queue only when the
    // transaction commits; an abort returns it to the queue.
    [[nodiscard]] result<received_message> receive(
        std::chrono::milliseconds timeout, transaction& tx);

    // Closes the handle. Idempotent; the destructor also closes it.
    [[nodiscard]] status close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    // The resolved format name (after MQPathNameToFormatName, when the path was
    // not already a format name).
    [[nodiscard]] const std::string& format_name() const noexcept;

private:
    friend class transaction;
    struct impl;
    explicit queue(std::unique_ptr<impl> pimpl) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace msmq
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_MSMQ_QUEUE_HPP