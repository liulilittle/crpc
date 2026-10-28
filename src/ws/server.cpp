// CRPC -- WebSocket server implementation (RFC 6455 section 4.2).
#include <crpc/ws/server.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <crpc/http/message.hpp>
#include <crpc/ws/handshake.hpp>

#include "transport.hpp"

namespace crpc {
namespace ws {
namespace {

// The failure answer a rejected Upgrade gets before the connection is dropped
// (RFC 6455 section 4.2.2 allows any status other than 101 to refuse).
constexpr char k_rejected_response[] =
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] bool wants_tls(const server_options& options) noexcept {
    return !options.tls.certificate_chain.empty() ||
           !options.tls.private_key.empty();
}

} // namespace

struct server::impl {
    backend_factory& backend;
    endpoint ep;
    server_options options;
    status bind_error{};
    std::unique_ptr<crpc::listener> listener;

    struct acceptor final : crpc::accept_sink {
        impl& owner;
        explicit acceptor(impl& owner_value) noexcept : owner(owner_value) {}
        void on_accept(std::unique_ptr<crpc::connection> connection) noexcept override {
            owner.enqueue(std::move(connection));
        }
    };

    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::unique_ptr<crpc::connection>> pending;
    bool stopped = false;
    bool running_flag = false;
    acceptor accept_sink;

    impl(backend_factory& b, endpoint e, const server_options& o)
        : backend(b), ep(std::move(e)), options(o), accept_sink(*this) {}

    void bind() {
        listener = backend.listen(ep, listen_options{});
        if (!listener) {
            bind_error = status{rpc_code::unavailable,
                                "the WebSocket listener could not bind the endpoint"};
            return;
        }
        listener->start(&accept_sink);
        running_flag = true;
    }

    void enqueue(std::unique_ptr<crpc::connection> connection) noexcept {
        if (!connection) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (stopped || pending.size() >= options.max_pending_accepts ||
            !running_flag) {
            connection->close();
            return;
        }
        pending.push_back(std::move(connection));
        cv.notify_one();
    }

    std::unique_ptr<crpc::connection> next() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return stopped || !pending.empty(); });
        if (stopped && pending.empty()) {
            return nullptr;
        }
        std::unique_ptr<crpc::connection> connection = std::move(pending.front());
        pending.pop_front();
        return connection;
    }
};

server::server(backend_factory& backend, endpoint ep, const server_options& options)
    : impl_(new impl(backend, std::move(ep), options)) {
    impl_->bind();
}

server::~server() { stop(); }

void server::stop() noexcept {
    if (!impl_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->stopped = true;
        impl_->running_flag = false;
    }
    impl_->cv.notify_all();
    if (impl_->listener) {
        impl_->listener->stop();
    }
}

bool server::running() const noexcept {
    return impl_ != nullptr && impl_->running_flag && !impl_->stopped;
}

endpoint server::bound_endpoint() const noexcept {
    return impl_ ? impl_->ep : endpoint();
}

result<std::unique_ptr<stream>> server::accept() {
    // `status` is truthy when it is OK, so a failed bind is `!bind_error`.
    if (!impl_->bind_error) {
        return impl_->bind_error.error();
    }
    if (!impl_->running_flag) {
        return rpc_error{rpc_code::unavailable,
                         "the WebSocket server is not running"};
    }
    for (;;) {
        std::unique_ptr<crpc::connection> connection = impl_->next();
        if (!connection) {
            return rpc_error{rpc_code::unavailable,
                             "the WebSocket server has stopped"};
        }

        std::unique_ptr<detail::transport_channel> channel;
        const bool tls = wants_tls(impl_->options);
        if (tls) {
#if defined(CRPC_HAVE_OPENSSL)
            io::tls_config config = impl_->options.tls;
            config.role = io::tls_role::server;
            // No ALPN protocol is defined for WebSocket.
            config.require_alpn = false;
            config.alpn_protocols.clear();
            std::error_code ec;
            std::unique_ptr<io::tls_context> context =
                io::tls_context::create(config, ec);
            if (!context || ec) {
                connection->close();
                return rpc_error{rpc_code::failed_precondition,
                                 "wss needs a usable TLS context"};
            }
            channel.reset(new detail::transport_channel(
                std::move(connection), *context, std::string(),
                impl_->options.operation_timeout));
            // `context` may go out of scope: the TLS stream retains the shared
            // context state for the lifetime of the channel.
#else
            connection->close();
            return rpc_error{rpc_code::failed_precondition,
                             "wss needs CRPC_WITH_OPENSSL=ON at build time"};
#endif
        } else {
            channel.reset(new detail::transport_channel(
                std::move(connection), impl_->options.operation_timeout));
        }

        http::http_parser parser(http::http_parser::kind::request,
                                 http::parser_limits{});
        std::array<std::byte, 8 * 1024> chunk{};
        span<const std::byte> pending;
        for (;;) {
            result<std::size_t> got =
                channel->read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                return got.error();
            }
            if (got.value() == 0) {
                continue;
            }
            const http::parse_status step =
                parser.feed(span<const std::byte>(chunk.data(), got.value()));
            if (step == http::parse_status::error) {
                (void)channel->write(span<const std::byte>(
                    reinterpret_cast<const std::byte*>(k_rejected_response),
                    sizeof(k_rejected_response) - 1));
                return parser.error();
            }
            if (step == http::parse_status::message_complete) {
                pending = parser.pending();
                break;
            }
        }

        result<std::string> key = validate_upgrade_request(parser.get_request());
        if (!key) {
            (void)channel->write(span<const std::byte>(
                reinterpret_cast<const std::byte*>(k_rejected_response),
                sizeof(k_rejected_response) - 1));
            continue; // serve the next client; the bad one is closed below
        }

        const http::response response = make_upgrade_response(key.value());
        std::string wire;
        const status encoded = http::serialize(response, wire);
        if (!encoded) {
            return encoded.error();
        }
        status sent = channel->write(span<const std::byte>(
            reinterpret_cast<const std::byte*>(wire.data()), wire.size()));
        if (!sent) {
            return sent.error();
        }

        std::string leftover(reinterpret_cast<const char*>(pending.data()),
                             pending.size());
        stream_options session_options = impl_->options.stream;
        session_options.is_client = false; // the server role fixes the mask direction
        return std::unique_ptr<stream>(new stream(std::move(channel),
                                                  session_options,
                                                  as_bytes(leftover)));
    }
}

} // namespace ws
} // namespace crpc