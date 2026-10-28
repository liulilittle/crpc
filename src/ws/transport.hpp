// CRPC -- the byte transport under a WebSocket session.
//
// A blocking `db::channel` over one `crpc::connection`, optionally routed
// through a TLS stream (crpc/io/tls.hpp). This is the same
// `crpc::db::stream_channel` the HTTPS client uses: one blocking TLS byte pump
// serves both `wss://` and `https://`, so the transport wiring, timeouts,
// backpressure and TLS error mapping cannot drift between the two protocols.
#ifndef CRPC_WS_TRANSPORT_HPP
#define CRPC_WS_TRANSPORT_HPP

#include <crpc/db/stream_channel.hpp>

namespace crpc {
namespace ws {
namespace detail {

using transport_channel = crpc::db::stream_channel;

} // namespace detail
} // namespace ws
} // namespace crpc

#endif // CRPC_WS_TRANSPORT_HPP