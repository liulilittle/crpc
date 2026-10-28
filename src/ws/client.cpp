// CRPC -- WebSocket client implementation (RFC 6455 sections 4.1-4.2.2).
#include <crpc/ws/client.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <crpc/http/message.hpp>
#include <crpc/ws/handshake.hpp>

#include "transport.hpp"

namespace crpc {
namespace ws {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

result<std::unique_ptr<detail::transport_channel>> establish_transport(
    backend_factory& backend, const endpoint& ep, const client_options& options) {
    crpc::connect_options copts;
    copts.connect_timeout = options.connect_timeout;
    std::unique_ptr<crpc::connection> raw = backend.connect(ep, copts);
    if (!raw) {
        return rpc_error{rpc_code::unavailable, "the WebSocket transport refused to connect"};
    }
    if (!options.tls) {
        return std::unique_ptr<detail::transport_channel>(
            new detail::transport_channel(std::move(raw), options.operation_timeout));
    }
#if defined(CRPC_HAVE_OPENSSL)
    io::tls_config config = options.tls_config;
    config.role = io::tls_role::client;
    // The WebSocket opening handshake is HTTP/1.1 Upgrade; no ALPN protocol is
    // defined for it, so the sender must not demand one.
    config.require_alpn = false;
    std::error_code ec;
    std::unique_ptr<io::tls_context> context = io::tls_context::create(config, ec);
    if (!context || ec) {
        raw->close();
        return rpc_error{rpc_code::failed_precondition,
                         "wss:// needs a usable TLS context: " + (ec ? ec.message() : std::string())};
    }
    return std::unique_ptr<detail::transport_channel>(
        new detail::transport_channel(std::move(raw), *context, options.host,
                                      options.operation_timeout));
    // `context` may be released here: the TLS stream retains the shared context
    // state for the lifetime of the channel.
#else
    raw->close();
    return rpc_error{rpc_code::failed_precondition,
                     "wss:// needs CRPC_WITH_OPENSSL=ON at build time"};
#endif
}

status read_response(detail::transport_channel& chan, const std::string& key,
                     std::string& leftover) {
    http::http_parser parser(http::http_parser::kind::response, http::parser_limits{});
    std::array<std::byte, 8 * 1024> chunk{};
    span<const std::byte> pending;
    for (;;) {
        result<std::size_t> got = chan.read_some(span<std::byte>(chunk.data(), chunk.size()));
        if (!got) {
            return status{got.error()};
        }
        if (got.value() == 0) {
            continue;
        }
        const http::parse_status step =
            parser.feed(span<const std::byte>(chunk.data(), got.value()));
        if (step == http::parse_status::error) {
            return status{parser.error()};
        }
        if (step == http::parse_status::message_complete) {
            pending = parser.pending();
            break;
        }
    }
    const status valid = validate_upgrade_response(parser.get_response(), key);
    if (!valid) {
        return valid;
    }
    leftover.assign(reinterpret_cast<const char*>(pending.data()), pending.size());
    return status{};
}

} // namespace

client::client(std::unique_ptr<stream> session) noexcept : session_(std::move(session)) {}

client::~client() = default;

result<event> client::recv() {
    if (!session_) {
        return rpc_error{rpc_code::shutting_down, "the WebSocket client has no session"};
    }
    return session_->recv();
}

status client::send_text(std::string_view text) {
    return session_ ? session_->send_text(text)
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

status client::send_binary(span<const std::byte> data) {
    return session_ ? session_->send_binary(data)
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

status client::send_ping(span<const std::byte> payload) {
    return session_ ? session_->send_ping(payload)
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

status client::send_pong(span<const std::byte> payload) {
    return session_ ? session_->send_pong(payload)
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

status client::send_close(close_code code, std::string_view reason) {
    return session_ ? session_->send_close(code, reason)
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

status client::close() {
    return session_ ? session_->close()
                    : status{rpc_code::shutting_down, "the WebSocket client has no session"};
}

void client::abort() noexcept {
    if (session_) {
        session_->abort();
    }
}

bool client::is_open() const noexcept {
    return session_ != nullptr && session_->is_open();
}

result<std::unique_ptr<client>> connect(backend_factory& backend, endpoint ep,
                                        client_options options) {
    const std::string key = websocket_key();
    std::string leftover;
    std::string host_header = options.host;
    if (options.port != 80 && options.port != 443) {
        host_header += ":";
        host_header += std::to_string(options.port);
    }

    result<std::unique_ptr<detail::transport_channel>> transport =
        establish_transport(backend, ep, options);
    if (!transport) {
        return transport.error();
    }

    const http::request request =
        make_upgrade_request(host_header, options.path, options.origin, key);
    std::string wire;
    const status encoded = http::serialize(request, wire);
    if (!encoded) {
        return encoded.error();
    }
    status written = transport.value()->write(
        span<const std::byte>(reinterpret_cast<const std::byte*>(wire.data()),
                              wire.size()));
    if (!written) {
        return written.error();
    }

    const status verified = read_response(*transport.value(), key, leftover);
    if (!verified) {
        return verified.error();
    }

    std::unique_ptr<detail::transport_channel> link = std::move(transport.value());
    stream_options session_options = options.stream;
    session_options.is_client = true; // the client role fixes the mask direction
    std::unique_ptr<stream> session(new stream(std::move(link), session_options,
                                               as_bytes(leftover)));
    return std::unique_ptr<client>(new client(std::move(session)));
}

} // namespace ws
} // namespace crpc