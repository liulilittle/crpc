// CRPC -- MSMQ local transaction.
//
// A `transaction` wraps one ITransaction obtained from MQBeginTransaction.
// Bind it to send/receive: messages sent inside a transaction are visible only
// after `commit`; a transactional receive removes its message from the queue
// only on commit, so `abort` returns it to the queue. Both operations map to
// the MSMQ semantics, which a real queue verifies.
//
// All methods are synchronous and value-returning; nothing here throws.
#ifndef CRPC_MQ_MSMQ_TRANSACTION_HPP
#define CRPC_MQ_MSMQ_TRANSACTION_HPP

#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace mq {
namespace msmq {

class CRPC_API transaction {
public:
    // Starts a new local transaction. May fail when COM or the MSMQ runtime is
    // unavailable on the host.
    [[nodiscard]] static result<std::unique_ptr<transaction>> begin();

    ~transaction();

    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;
    transaction(transaction&&) = delete;
    transaction& operator=(transaction&&) = delete;

    // Commits all operations bound to this transaction. Returns
    // `failed_precondition` when the transaction is finished.
    [[nodiscard]] status commit();

    // Rolls the transaction back: committed-at-commit changes are undone, and
    // a transactionally received message is returned to its queue. Idempotent.
    [[nodiscard]] status abort();

    [[nodiscard]] bool is_active() const noexcept;

private:
    friend void* transaction_handle(transaction& tx) noexcept;
    struct impl;
    explicit transaction(std::unique_ptr<impl> pimpl) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace msmq
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_MSMQ_TRANSACTION_HPP