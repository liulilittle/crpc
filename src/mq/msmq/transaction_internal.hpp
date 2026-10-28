// CRPC -- internal accessor so queue.cpp can bind a receive/send to the
// transaction's native ITransaction pointer without knowing its layout.
//
// Not part of the public API. The value is an opaque pointer that only the two
// MSMQ implementation translation units reinterpret.
#ifndef CRPC_MQ_MSMQ_TRANSACTION_INTERNAL_HPP
#define CRPC_MQ_MSMQ_TRANSACTION_INTERNAL_HPP

#include <crpc/config.hpp>
#include <crpc/mq/msmq/transaction.hpp>

namespace crpc {
namespace mq {
namespace msmq {

// Returns the native MSMQ transaction handle (Windows: ITransaction*) or
// nullptr when the transaction is finished or not backed.
[[nodiscard]] void* transaction_handle(transaction& tx) noexcept;

} // namespace msmq
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_MSMQ_TRANSACTION_INTERNAL_HPP