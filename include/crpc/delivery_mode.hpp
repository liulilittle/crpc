// CRPC -- delivery guarantee vocabulary.
//
// A request can be delivered reliably (request/response, the call carries a
// deadline and an idempotency key) or as a fire-and-forget ONE_WAY message.
// The RPC core and the push hub both need this two-value guarantee, and before
// this header each declared its own `delivery_mode` enum with the same members.
// Two distinct types with the same spelling cannot interoperate: a `rpc` caller
// that wanted to forward a push mode, or a push publisher that wanted to reuse
// an rpc mode, had to know which namespace it was in.
//
// The enum is defined once here and aliased into both namespaces, so
// `crpc::rpc::delivery_mode` and `crpc::push::delivery_mode` are the same type
// and the values pass freely between the two. The values are wire-visible
// (the PUSH envelope carries the mode as a u8) and are append-only.
#ifndef CRPC_DELIVERY_MODE_HPP
#define CRPC_DELIVERY_MODE_HPP

#include <cstdint>

#include <crpc/config.hpp>

namespace crpc {

// Best-effort or acknowledged delivery. Reliability is a property of the
// (subscription, message) pair: a message is reliable only when both the
// subscription and the publish are reliable.
enum class delivery_mode : u8 {
    reliable = 0,
    unreliable = 1
};

} // namespace crpc

namespace crpc::rpc {
using crpc::delivery_mode;
} // namespace crpc::rpc

namespace crpc::push {
using crpc::delivery_mode;
} // namespace crpc::push

#endif // CRPC_DELIVERY_MODE_HPP
