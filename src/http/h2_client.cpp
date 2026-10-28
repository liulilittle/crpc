// CRPC -- HTTP/2 multiplexing client facade (see include/crpc/http/h2_client.hpp).
#include <crpc/http/h2_client.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <crpc/db/stream_channel.hpp>
#include <crpc/io/tls.hpp>

#include "h2_session.hpp"

namespace crpc {
namespace http {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

// The `:authority` value: host, plus the port when it is not the scheme's
// default (the same rule the HTTP/1.1 client applies to `Host`).
[[nodiscard]] std::string authority_for(const h2_client_options& options) {
    std::string authority = options.host;
    const std::uint16_t default_port = options.tls ? 443 : 80;
    if (options.port != default_port) {
        authority += ':';
        authority += std::to_string(options.port);
    }
    return authority;
}

[[nodiscard]] detail::h2_session_config session_config(const h2_client_options& options) {
    detail::h2_session_config config;
    config.authority = authority_for(options);
    config.scheme = options.tls ? "https" : "http";
    config.default_headers = options.default_headers;
    config.max_header_bytes = options.max_header_bytes;
    config.max_body_bytes = options.max_body_bytes;
    config.initial_window_size = options.h2_initial_window_size;
    config.max_frame_size = options.h2_max_frame_size;
    config.max_send_queue_bytes = options.h2_max_send_queue_bytes;
    return config;
}

[[nodiscard]] std::chrono::milliseconds timeout_budget(const h2_client_options& options) {
    return options.timeout_seconds == 0
               ? std::chrono::milliseconds(0)
               : std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::seconds(options.timeout_seconds));
}

} // namespace

struct h2_client::impl {
    h2_client_options options;
    std::unique_ptr<db::channel> link;
    std::unique_ptr<detail::h2_session> session;
};

h2_client::h2_client(h2_client_options options, std::unique_ptr<db::channel> link)
    : impl_(new impl()) {
    impl_->options = std::move(options);
    impl_->link = std::move(link);
    impl_->session.reset(new detail::h2_session(*impl_->link, session_config(impl_->options)));
}

h2_client::~h2_client() { close(); }

result<h2_stream> h2_client::begin(method verb, std::string_view target,
                                   span<const std::byte> body,
                                   span<const header> extra_headers) {
    if (!impl_ || !impl_->link) {
        return rpc_error{rpc_code::shutting_down, "the HTTP/2 client has no link"};
    }
    return impl_->session->begin(verb, target, body, extra_headers);
}

result<response> h2_client::finish(h2_stream stream) {
    if (!impl_ || !impl_->link) {
        return rpc_error{rpc_code::shutting_down, "the HTTP/2 client has no link"};
    }
    return impl_->session->finish(stream);
}

result<response> h2_client::send(method verb, std::string_view target,
                                 span<const std::byte> body,
                                 span<const header> extra_headers) {
    const result<h2_stream> opened = begin(verb, target, body, extra_headers);
    if (!opened) {
        return opened.error();
    }
    return finish(opened.value());
}

result<response> h2_client::send_text(method verb, std::string_view target,
                                      std::string_view body, std::string_view content_type) {
    header fields[1];
    if (!content_type.empty()) {
        fields[0].name = "Content-Type";
        fields[0].value = std::string(content_type);
    }
    return send(verb, target, as_bytes(body),
                span<const header>(fields, content_type.empty() ? 0 : 1));
}

void h2_client::close() noexcept {
    if (impl_ && impl_->session) {
        impl_->session->close();
    }
}

bool h2_client::is_open() const noexcept {
    // A `db::channel` has no generic liveness query: "has a usable link" is
    // what this client can answer honestly, mirroring the HTTP/1.1 client.
    return impl_ != nullptr && impl_->link != nullptr;
}

result<std::unique_ptr<h2_client>> make_h2_client(h2_client_options options,
                                                  crpc::backend_factory& backend,
                                                  crpc::endpoint endpoint,
                                                  crpc::runtime* shard) {
    if (!options.tls) {
        crpc::net::connection_config config;
        config.ep = std::move(endpoint);
        // h2c prior knowledge has no fallback: a closed link is terminal.
        config.reconnect_on_eof = false;
        if (options.timeout_seconds != 0) {
            config.operation_timeout = std::chrono::seconds(options.timeout_seconds);
        }
        auto link = std::unique_ptr<db::channel>(
            new db::socket_channel(std::move(config), backend, shard));
        return result<std::unique_ptr<h2_client>>{std::unique_ptr<h2_client>(
            new h2_client(std::move(options), std::move(link)))};
    }

#if !defined(CRPC_HAVE_OPENSSL)
    return result<std::unique_ptr<h2_client>>{rpc_error{
        rpc_code::failed_precondition,
        "https:// requires the OpenSSL TLS backend: rebuild the library with "
        "-DCRPC_WITH_OPENSSL=ON"}};
#else
    const std::string sni =
        options.tls_server_name.empty() ? options.host : options.tls_server_name;
    if (sni.empty()) {
        return result<std::unique_ptr<h2_client>>{rpc_error{
            rpc_code::failed_precondition,
            "https:// requires a host name for SNI and certificate verification"}};
    }

    io::tls_config tls;
    tls.role = io::tls_role::client;
    tls.verify = options.tls_verify_peer ? io::tls_verify_mode::peer
                                         : io::tls_verify_mode::none;
    tls.ca_bundle = options.tls_ca_bundle;
    tls.alpn_protocols = options.tls_alpn;
    tls.require_alpn = false;
    // The factory itself enforces the outcome: only a negotiated "h2" yields
    // a client; anything else is reported so the caller can fall back to
    // http::make_client (which speaks HTTP/1.1).

    std::error_code ec;
    std::unique_ptr<io::tls_context> context = io::tls_context::create(tls, ec);
    if (!context || ec) {
        return result<std::unique_ptr<h2_client>>{rpc_error{
            rpc_code::failed_precondition,
            "HTTPS needs a usable TLS context: " +
                (ec ? ec.message() : std::string("unknown error"))}};
    }

    const std::chrono::milliseconds budget = timeout_budget(options);
    crpc::connect_options connect;
    if (options.timeout_seconds != 0) {
        connect.connect_timeout = std::chrono::seconds(options.timeout_seconds);
    }
    std::unique_ptr<crpc::connection> raw = backend.connect(endpoint, connect);
    if (!raw) {
        return result<std::unique_ptr<h2_client>>{rpc_error{
            rpc_code::unavailable, "the HTTPS transport refused to connect"}};
    }
    auto* stream = new db::stream_channel(std::move(raw), *context, sni, budget);
    std::unique_ptr<db::channel> link(stream);

    // h2 over TLS requires ALPN: the handshake must settle before the first
    // byte of the connection preface goes out.
    const status handshook = stream->wait_handshake(budget);
    if (!handshook.ok()) {
        return result<std::unique_ptr<h2_client>>{handshook.error()};
    }
    const std::string negotiated = stream->tls_alpn_protocol();
    if (negotiated != "h2") {
        return result<std::unique_ptr<h2_client>>{rpc_error{
            rpc_code::failed_precondition,
            "the server negotiated '" + (negotiated.empty() ? std::string("(none)")
                                                            : negotiated) +
                "', not h2: use http::make_client for the HTTP/1.1 fallback"}};
    }
    return result<std::unique_ptr<h2_client>>{std::unique_ptr<h2_client>(
        new h2_client(std::move(options), std::move(link)))};
#endif
}

} // namespace http
} // namespace crpc
