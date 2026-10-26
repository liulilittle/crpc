// CRPC Redis -- compatibility names for the shared byte transport.
//
// The connection and pool used by the Redis client are protocol-agnostic, so
// they now live in `crpc/net/connection.hpp` and are shared with the other
// protocol clients. This header keeps the `crpc::redis` spellings as aliases
// rather than type definitions, so existing call sites and the exported Redis
// signatures keep referring to exactly the same types.
#ifndef CRPC_REDIS_CONNECTION_HPP
#define CRPC_REDIS_CONNECTION_HPP

#include <crpc/net/connection.hpp>

namespace crpc {
namespace redis {

using connection = crpc::net::connection;
using connection_config = crpc::net::connection_config;
using connection_pool = crpc::net::connection_pool;
using pool_config = crpc::net::pool_config;

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_CONNECTION_HPP
